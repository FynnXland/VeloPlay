#include "render/VideoRenderer.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include <d3dcompiler.h>

namespace veloplay::render {

using Microsoft::WRL::ComPtr;

namespace {

// One triangle that covers the viewport; no vertex buffer needed.
const char kVertexShader[] = R"(
struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Output main(uint id : SV_VertexID) {
    Output o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.uv = p;
    o.position = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
)";

const char kPixelShader[] = R"(
Texture2D lumaTexture : register(t0);
Texture2D chromaTexture : register(t1);
SamplerState linearSampler : register(s0);
cbuffer Color : register(b0) { float4 row0; float4 row1; float4 row2; };
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float y = lumaTexture.Sample(linearSampler, uv).r;
    float2 c = chromaTexture.Sample(linearSampler, uv).rg;
    float4 yuv = float4(y, c.x, c.y, 1.0);
    return float4(saturate(float3(dot(row0, yuv), dot(row1, yuv), dot(row2, yuv))), 1.0);
}
)";

// A small copy of the brightness, for telling a repeated picture from a new
// one. Four samples a quarter of a cell apart; each reads four pixels.
const char kProbeShader[] = R"(
Texture2D lumaTexture : register(t0);
SamplerState linearSampler : register(s0);
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 d = float2(0.25 / 96.0, 0.25 / 54.0);
    float y = lumaTexture.SampleLevel(linearSampler, uv + float2(-d.x, -d.y), 0).r +
              lumaTexture.SampleLevel(linearSampler, uv + float2(d.x, -d.y), 0).r +
              lumaTexture.SampleLevel(linearSampler, uv + float2(-d.x, d.y), 0).r +
              lumaTexture.SampleLevel(linearSampler, uv + float2(d.x, d.y), 0).r;
    return float4(y * 0.25, 0.0, 0.0, 1.0);
}
)";

// Stage three, soft: the card's own linear filter.
const char kSoftShader[] = R"(
Texture2D source : register(t0);
SamplerState linearSampler : register(s0);
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return float4(source.SampleLevel(linearSampler, uv, 0).rgb, 1.0);
}
)";

#define VELOPLAY_OUTPUT_CONSTANTS                                                                                      \
    "cbuffer Output : register(b0) {\n"                                                                                \
    "    float2 sourceSize;  // size of the texture that is read, in pixels\n"                                         \
    "    float2 origin;      // top left corner of the picture inside the render target\n"                             \
    "    float sharpness;\n"                                                                                           \
    "    float3 unused;\n"                                                                                             \
    "};\n"

// Stage three, sharp: Lanczos with two lobes, sixteen samples. Enlarging
// with it keeps edges crisper than the linear filter. Its side effect is a
// faint echo next to hard edges; limiting the result to what the four
// nearest pixels span removes that.
const char kLanczosShader[] = VELOPLAY_OUTPUT_CONSTANTS R"(
Texture2D source : register(t0);
SamplerState pointSampler : register(s1);
float weight(float x) {
    x = abs(x);
    if (x < 0.0001) return 1.0;
    if (x >= 2.0) return 0.0;
    float px = 3.14159265 * x;
    return sin(px) * sin(px * 0.5) / (px * px * 0.5);
}
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 p = uv * sourceSize - 0.5;
    float2 base = floor(p);
    float2 f = p - base;
    float3 sum = float3(0.0, 0.0, 0.0);
    float total = 0.0;
    float3 lowest = float3(1.0, 1.0, 1.0);
    float3 highest = float3(0.0, 0.0, 0.0);
    [unroll] for (int y = -1; y <= 2; ++y) {
        [unroll] for (int x = -1; x <= 2; ++x) {
            float w = weight(float(x) - f.x) * weight(float(y) - f.y);
            float3 c = source.SampleLevel(pointSampler, (base + float2(x, y) + 0.5) / sourceSize, 0).rgb;
            sum += c * w;
            total += w;
            if (x >= 0 && x <= 1 && y >= 0 && y <= 1) {
                lowest = min(lowest, c);
                highest = max(highest, c);
            }
        }
    }
    return float4(clamp(sum / total, lowest, highest), 1.0);
}
)";

// Sharpening after the method AMD published as RCAS in FidelityFX Super
// Resolution 1 (MIT licence): each pixel is pushed away from the average of
// its four neighbours, but only as far as the contrast around it allows, so
// edges get crisper without bright or dark fringes. Written from the
// description; see THIRD_PARTY.md.
const char kSharpenShader[] = VELOPLAY_OUTPUT_CONSTANTS R"(
Texture2D source : register(t0);
float3 at(int2 p) {
    int2 limit = int2(sourceSize) - 1;
    return source.Load(int3(clamp(p, int2(0, 0), limit), 0)).rgb;
}
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    int2 p = int2(position.xy - origin);
    float3 e = at(p);
    float3 b = at(p + int2(0, -1));
    float3 d = at(p + int2(-1, 0));
    float3 f = at(p + int2(1, 0));
    float3 h = at(p + int2(0, 1));
    float3 lowest = min(min(b, d), min(f, h));
    float3 highest = max(max(b, d), max(f, h));
    float3 roomDown = min(lowest, e) / (4.0 * highest + 0.00001);
    float3 roomUp = (1.0 - max(highest, e)) / min(4.0 * lowest - 4.0, -0.00001);
    float3 lobes = max(-roomDown, roomUp);
    float lobe = max(-0.1875, min(max(lobes.r, max(lobes.g, lobes.b)), 0.0)) * sharpness;
    return float4(saturate((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0)), 1.0);
}
)";

bool compile(const char* source, std::size_t length, const char* target, ComPtr<ID3DBlob>& code, std::string& error) {
    ComPtr<ID3DBlob> messages;
    const HRESULT result = D3DCompile(source, length, nullptr, nullptr, nullptr, "main", target, 0, 0,
                                      code.GetAddressOf(), messages.GetAddressOf());
    if (SUCCEEDED(result)) return true;
    error = "shader did not compile";
    if (messages) {
        error += ": ";
        error.append(static_cast<const char*>(messages->GetBufferPointer()), messages->GetBufferSize());
    }
    return false;
}

struct Constants {
    float rows[3][4];
};

struct OutputConstants {
    float sourceSize[2];
    float origin[2];
    float sharpness;
    float unused[3];
};

bool makePixelShader(ID3D11Device* device, const char* source, std::size_t length, ComPtr<ID3D11PixelShader>& shader,
                     std::string& error) {
    ComPtr<ID3DBlob> code;
    if (!compile(source, length, "ps_4_0", code, error)) return false;
    if (FAILED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                         shader.GetAddressOf()))) {
        error = "pixel shader could not be created";
        return false;
    }
    return true;
}

} // namespace

bool VideoRenderer::create(ID3D11Device* device, ID3D11DeviceContext* context, std::string& error) {
    device_ = device;
    context_ = context;

    ComPtr<ID3DBlob> vertexCode;
    if (!compile(kVertexShader, sizeof(kVertexShader) - 1, "vs_4_0", vertexCode, error)) return false;
    if (FAILED(device_->CreateVertexShader(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr,
                                           vertexShader_.GetAddressOf()))) {
        error = "shaders could not be created";
        return false;
    }
    if (!makePixelShader(device, kPixelShader, sizeof(kPixelShader) - 1, convertShader_, error) ||
        !makePixelShader(device, kSoftShader, sizeof(kSoftShader) - 1, softShader_, error) ||
        !makePixelShader(device, kLanczosShader, sizeof(kLanczosShader) - 1, lanczosShader_, error) ||
        !makePixelShader(device, kSharpenShader, sizeof(kSharpenShader) - 1, sharpenShader_, error)) {
        return false;
    }

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;

    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;

    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(Constants);
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    D3D11_SAMPLER_DESC point = sampler;
    point.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    D3D11_BUFFER_DESC outputBuffer = buffer;
    outputBuffer.ByteWidth = sizeof(OutputConstants);

    if (FAILED(device_->CreateSamplerState(&sampler, sampler_.GetAddressOf())) ||
        FAILED(device_->CreateSamplerState(&point, pointSampler_.GetAddressOf())) ||
        FAILED(device_->CreateRasterizerState(&rasterizer, rasterizer_.GetAddressOf())) ||
        FAILED(device_->CreateBuffer(&buffer, nullptr, colorConstants_.GetAddressOf())) ||
        FAILED(device_->CreateBuffer(&outputBuffer, nullptr, outputConstants_.GetAddressOf()))) {
        error = "render states could not be created";
        return false;
    }

    // So is the comparison of pictures; without it a halved game rate goes unnoticed.
    std::string probeError;
    if (!makePixelShader(device, kProbeShader, sizeof(kProbeShader) - 1, probeShader_, probeError)) probeShader_.Reset();

    // Frame generation is an extra. If the card cannot set it up, pictures
    // are shown as they arrive and the reason is kept for the log.
    auto generator = std::make_unique<FlowFrameGenerator>();
    if (generator->create(device, context, generatorError_)) generator_ = std::move(generator);
    // So is NVIDIA's scaler; other cards use the Lanczos shader.
    auto rtx = std::make_unique<RtxVideoScaler>();
    if (rtx->create(device, context, rtxError_)) rtx_ = std::move(rtx);
    return true;
}

void VideoRenderer::clear() {
    hasPicture_ = false;
    lastGenerated_ = false;
    slots_[0].id = 0;
    slots_[1].id = 0;
    prepared_[0] = false;
    prepared_[1] = false;
    generatedFor_ = {};
    for (ProbeCopy& copy : probeCopies_) copy.pending = false;
    probeBefore_.clear();
    probeBeforePicture_ = 0;
    changes_.clear();
}

std::optional<VideoRenderer::Change> VideoRenderer::takeChange() {
    if (changes_.empty()) return std::nullopt;
    const Change change = changes_.front();
    changes_.pop_front();
    return change;
}

void VideoRenderer::probe() {
    if (!probeShader_ || !lumaView_ || !ensureTarget(probeTarget_, kProbeWidth, kProbeHeight)) return;
    ProbeCopy& copy = probeCopies_[submitted_ % std::size(probeCopies_)];

    // What this place holds is three pictures old. Read it before it is written again.
    if (copy.pending && copy.staging) {
        copy.pending = false;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context_->Map(copy.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
            std::vector<std::uint8_t> now(static_cast<std::size_t>(kProbeWidth) * kProbeHeight);
            for (int y = 0; y < kProbeHeight; ++y) {
                const auto* row = static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch;
                for (int x = 0; x < kProbeWidth; ++x) now[static_cast<std::size_t>(y * kProbeWidth + x)] = row[x * 4];
            }
            context_->Unmap(copy.staging.Get(), 0);
            if (probeBeforePicture_ + 1 == copy.picture && probeBefore_.size() == now.size()) {
                // A repeat that went through the encoder again differs by a level or two
                // here and there. More than three levels is a real change.
                std::size_t changed = 0;
                for (std::size_t i = 0; i < now.size(); ++i) {
                    if (std::abs(static_cast<int>(now[i]) - static_cast<int>(probeBefore_[i])) > 3) ++changed;
                }
                changes_.push_back({copy.picture, static_cast<double>(changed) / static_cast<double>(now.size())});
                if (changes_.size() > 64) changes_.pop_front();
            }
            probeBefore_ = std::move(now);
            probeBeforePicture_ = copy.picture;
        }
    }

    if (!copy.staging) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = kProbeWidth;
        desc.Height = kProbeHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, copy.staging.GetAddressOf()))) return;
    }
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(kProbeWidth);
    viewport.Height = static_cast<float>(kProbeHeight);
    viewport.MaxDepth = 1.0f;
    ID3D11ShaderResourceView* views[1] = {lumaView_.Get()};
    pass(probeShader_.Get(), probeTarget_.target.Get(), viewport, views, 1, colorConstants_.Get());
    ID3D11RenderTargetView* noTarget = nullptr;
    context_->OMSetRenderTargets(1, &noTarget, nullptr);
    context_->CopyResource(copy.staging.Get(), probeTarget_.texture.Get());
    copy.picture = submitted_;
    copy.pending = true;
}

bool VideoRenderer::ensureTarget(Target& target, int width, int height) {
    if (target.texture && target.width == width && target.height == height) return true;
    target = Target{};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, target.texture.GetAddressOf())) ||
        FAILED(device_->CreateShaderResourceView(target.texture.Get(), nullptr, target.view.GetAddressOf())) ||
        FAILED(device_->CreateRenderTargetView(target.texture.Get(), nullptr, target.target.GetAddressOf()))) {
        target = Target{};
        return false;
    }
    target.width = width;
    target.height = height;
    return true;
}

void VideoRenderer::pass(ID3D11PixelShader* shader, ID3D11RenderTargetView* target, const D3D11_VIEWPORT& viewport,
                         ID3D11ShaderResourceView* const* views, UINT viewCount, ID3D11Buffer* constants) {
    ID3D11SamplerState* samplers[2] = {sampler_.Get(), pointSampler_.Get()};
    ID3D11Buffer* buffers[1] = {constants};
    const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    if (target) context_->OMSetRenderTargets(1, &target, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->RSSetState(rasterizer_.Get());
    context_->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
    context_->OMSetDepthStencilState(nullptr, 0);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->GSSetShader(nullptr, nullptr, 0);
    context_->PSSetShader(shader, nullptr, 0);
    context_->PSSetShaderResources(0, viewCount, views);
    context_->PSSetSamplers(0, 2, samplers);
    context_->PSSetConstantBuffers(0, 1, buffers);
    context_->Draw(3, 0);

    // Nothing stays bound: the next copy from the decoder and the next pass write to what was read here.
    ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
    context_->PSSetShaderResources(0, 2, none);
}

bool VideoRenderer::ensureTextures(int width, int height, bool onCard) {
    if (width == width_ && height == height_ && onCard == onCard_ && lumaView_ && chromaView_) return true;

    lumaTexture_.Reset();
    chromaTexture_.Reset();
    nv12Texture_.Reset();
    lumaView_.Reset();
    chromaView_.Reset();
    clear();
    width_ = width;
    height_ = height;
    onCard_ = onCard;

    D3D11_TEXTURE2D_DESC desc{};
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;

    if (onCard) {
        // NV12 needs even dimensions.
        desc.Width = static_cast<UINT>(width + (width & 1));
        desc.Height = static_cast<UINT>(height + (height & 1));
        desc.Format = DXGI_FORMAT_NV12;
        desc.Usage = D3D11_USAGE_DEFAULT;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, nv12Texture_.GetAddressOf()))) return false;
        // The two planes of an NV12 texture are reached through views with
        // one-channel and two-channel formats.
        view.Format = DXGI_FORMAT_R8_UNORM;
        if (FAILED(device_->CreateShaderResourceView(nv12Texture_.Get(), &view, lumaView_.GetAddressOf()))) return false;
        view.Format = DXGI_FORMAT_R8G8_UNORM;
        return SUCCEEDED(device_->CreateShaderResourceView(nv12Texture_.Get(), &view, chromaView_.GetAddressOf()));
    }

    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.Format = DXGI_FORMAT_R8_UNORM;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, lumaTexture_.GetAddressOf()))) return false;
    desc.Width = static_cast<UINT>((width + 1) / 2);
    desc.Height = static_cast<UINT>((height + 1) / 2);
    desc.Format = DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, chromaTexture_.GetAddressOf()))) return false;

    view.Format = DXGI_FORMAT_R8_UNORM;
    if (FAILED(device_->CreateShaderResourceView(lumaTexture_.Get(), &view, lumaView_.GetAddressOf()))) return false;
    view.Format = DXGI_FORMAT_R8G8_UNORM;
    return SUCCEEDED(device_->CreateShaderResourceView(chromaTexture_.Get(), &view, chromaView_.GetAddressOf()));
}

bool VideoRenderer::upload(const media::VideoFrame& frame) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(lumaTexture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
    for (int row = 0; row < frame.height; ++row) {
        std::memcpy(static_cast<std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(row) * mapped.RowPitch,
                    frame.planes[0] + static_cast<std::ptrdiff_t>(row) * frame.strides[0],
                    static_cast<std::size_t>(frame.width));
    }
    context_->Unmap(lumaTexture_.Get(), 0);

    const int chromaWidth = (frame.width + 1) / 2;
    const int chromaHeight = (frame.height + 1) / 2;
    if (FAILED(context_->Map(chromaTexture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
    for (int row = 0; row < chromaHeight; ++row) {
        auto* out = static_cast<std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(row) * mapped.RowPitch;
        if (frame.layout == media::PixelLayout::Nv12) {
            std::memcpy(out, frame.planes[1] + static_cast<std::ptrdiff_t>(row) * frame.strides[1],
                        static_cast<std::size_t>(chromaWidth) * 2);
        } else {
            const std::uint8_t* u = frame.planes[1] + static_cast<std::ptrdiff_t>(row) * frame.strides[1];
            const std::uint8_t* v = frame.planes[2] + static_cast<std::ptrdiff_t>(row) * frame.strides[2];
            for (int x = 0; x < chromaWidth; ++x) {
                out[x * 2] = u[x];
                out[x * 2 + 1] = v[x];
            }
        }
    }
    context_->Unmap(chromaTexture_.Get(), 0);
    return true;
}

bool VideoRenderer::submit(const media::VideoFrame& frame, bool keep) {
    ++submitted_;
    if (!device_ || frame.width <= 0 || frame.height <= 0) return false;
    const bool onCard = frame.layout == media::PixelLayout::D3D11Nv12;
    if (!ensureTextures(frame.width, frame.height, onCard)) {
        lumaView_.Reset(); // forces a new attempt with the next picture
        return false;
    }

    if (onCard) {
        auto* source = static_cast<ID3D11Texture2D*>(frame.d3d11Texture);
        if (!source) return false;
        // The decoder's surface can be larger than the picture (1088 lines for 1080).
        D3D11_BOX box{};
        box.right = static_cast<UINT>(frame.width + (frame.width & 1));
        box.bottom = static_cast<UINT>(frame.height + (frame.height & 1));
        box.back = 1;
        context_->CopySubresourceRegion(nv12Texture_.Get(), 0, 0, 0, 0, source, static_cast<UINT>(frame.d3d11Index), &box);
    } else if (!upload(frame)) {
        return false;
    }
    probe();
    if (!keep) return true;

    if (!colorSet_ || !(color_ == frame.color)) {
        const media::ColorMatrix matrix = media::yuvToRgb(frame.color);
        Constants constants{};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) constants.rows[r][c] = matrix.rows[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
        }
        context_->UpdateSubresource(colorConstants_.Get(), 0, nullptr, &constants, 0, 0);
        color_ = frame.color;
        colorSet_ = true;
    }

    // Stage one: to RGB, into the slot that holds the older of the two pictures.
    const int slot = slots_[newest_].id == 0 ? newest_ : 1 - newest_;
    if (!ensureTarget(slots_[slot].rgb, width_, height_)) return false;
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width_);
    viewport.Height = static_cast<float>(height_);
    viewport.MaxDepth = 1.0f;
    ID3D11ShaderResourceView* views[2] = {lumaView_.Get(), chromaView_.Get()};
    pass(convertShader_.Get(), slots_[slot].rgb.target.Get(), viewport, views, 2, colorConstants_.Get());
    ID3D11RenderTargetView* noTarget = nullptr;
    context_->OMSetRenderTargets(1, &noTarget, nullptr);

    slots_[slot].id = nextId_++;
    newest_ = slot;
    prepared_[slot] = false;
    if (generator_ && generationWanted_) {
        if (generator_->prepare(slot, slots_[slot].rgb.view.Get(), width_, height_)) {
            prepared_[slot] = true;
        } else {
            generatorError_ = "the textures for frame generation could not be created";
            generator_.reset();
        }
    }
    hasPicture_ = true;
    return true;
}

ID3D11ShaderResourceView* VideoRenderer::pictureFor(const media::FrameChoice& choice) {
    lastGenerated_ = false;
    const Slot& newer = slots_[newest_];
    const Slot& older = slots_[1 - newest_];
    if (!choice.valid) return newer.rgb.view.Get();

    if (choice.step > 0 && choice.steps > 1 && generator_ && older.id != 0 && choice.base == older.id &&
        choice.next == newer.id && prepared_[0] && prepared_[1]) {
        // Stage two. A computed picture is kept until another one is asked for.
        if (!(generatedFor_ == choice) || !generated_.texture) {
            if (!ensureTarget(generated_, width_, height_)) return newer.rgb.view.Get();
            const float phase = static_cast<float>(choice.step) / static_cast<float>(choice.steps);
            if (!generator_->generate(1 - newest_, older.rgb.view.Get(), newest_, newer.rgb.view.Get(), phase,
                                      generated_.target.Get())) {
                return older.rgb.view.Get();
            }
            generatedFor_ = choice;
        }
        lastGenerated_ = true;
        return generated_.view.Get();
    }
    if (older.id != 0 && choice.base == older.id) return older.rgb.view.Get();
    return newer.rgb.view.Get();
}

void VideoRenderer::draw(UINT targetWidth, UINT targetHeight, const Output& output) {
    if (!hasPicture_ || targetWidth == 0 || targetHeight == 0) return;

    // The caller's render target comes back at the end; the stages in between draw elsewhere.
    ComPtr<ID3D11RenderTargetView> screen;
    ComPtr<ID3D11DepthStencilView> depth;
    context_->OMGetRenderTargets(1, screen.GetAddressOf(), depth.GetAddressOf());

    ID3D11ShaderResourceView* picture = pictureFor(output.choice);

    const float scale = std::min(static_cast<float>(targetWidth) / static_cast<float>(width_),
                                 static_cast<float>(targetHeight) / static_cast<float>(height_));
    // Whole pixels, so the sharpening pass can address its neighbours exactly.
    const int shownWidth = std::max(1, std::min(static_cast<int>(targetWidth), static_cast<int>(static_cast<float>(width_) * scale + 0.5f)));
    const int shownHeight = std::max(1, std::min(static_cast<int>(targetHeight), static_cast<int>(static_cast<float>(height_) * scale + 0.5f)));
    shownWidth_ = shownWidth;
    shownHeight_ = shownHeight;
    D3D11_VIEWPORT onScreen{};
    onScreen.Width = static_cast<float>(shownWidth);
    onScreen.Height = static_cast<float>(shownHeight);
    onScreen.TopLeftX = static_cast<float>((static_cast<int>(targetWidth) - shownWidth) / 2);
    onScreen.TopLeftY = static_cast<float>((static_cast<int>(targetHeight) - shownHeight) / 2);
    onScreen.MaxDepth = 1.0f;

    OutputConstants constants{};
    constants.sourceSize[0] = static_cast<float>(width_);
    constants.sourceSize[1] = static_cast<float>(height_);
    // Lanczos is for enlarging. Shrinking with it would skip pixels; the linear filter does that better.
    const bool lanczos = output.upscale && (shownWidth > width_ || shownHeight > height_);
    ID3D11PixelShader* scaler = lanczos ? lanczosShader_.Get() : softShader_.Get();
    const float sharpness = std::clamp(output.sharpness, 0.0f, 1.0f);
    scalerName_ = lanczos ? "Lanczos" : "";

    ID3D11RenderTargetView* screenTarget = screen.Get();
    ID3D11ShaderResourceView* enlarged = nullptr;
    if (lanczos && output.rtx && rtx_) {
        // NVIDIA's network does the enlarging. If the driver refuses, it is not asked again.
        ComPtr<ID3D11Resource> resource;
        ComPtr<ID3D11Texture2D> texture;
        picture->GetResource(resource.GetAddressOf());
        if (resource && SUCCEEDED(resource.As(&texture))) {
            enlarged = rtx_->scale(texture.Get(), width_, height_, shownWidth, shownHeight);
        }
        if (!enlarged) {
            rtxError_ = rtx_->error().empty() ? "the picture could not be handed to the video processor" : rtx_->error();
            rtx_.reset();
        }
    }
    if (enlarged) {
        scalerName_ = "RTX Video";
        constants.sourceSize[0] = static_cast<float>(shownWidth);
        constants.sourceSize[1] = static_cast<float>(shownHeight);
        constants.origin[0] = onScreen.TopLeftX;
        constants.origin[1] = onScreen.TopLeftY;
        constants.sharpness = sharpness;
        context_->UpdateSubresource(outputConstants_.Get(), 0, nullptr, &constants, 0, 0);
        context_->OMSetRenderTargets(1, &screenTarget, depth.Get());
        // Already at its final size: copied as it is, or sharpened on the way.
        pass(sharpness > 0.0f ? sharpenShader_.Get() : softShader_.Get(), nullptr, onScreen, &enlarged, 1,
             outputConstants_.Get());
    } else if (sharpness > 0.0f && ensureTarget(scaled_, shownWidth, shownHeight)) {
        // Stage three in two steps: to window size first, then sharpen onto the screen.
        D3D11_VIEWPORT whole{};
        whole.Width = static_cast<float>(shownWidth);
        whole.Height = static_cast<float>(shownHeight);
        whole.MaxDepth = 1.0f;
        context_->UpdateSubresource(outputConstants_.Get(), 0, nullptr, &constants, 0, 0);
        pass(scaler, scaled_.target.Get(), whole, &picture, 1, outputConstants_.Get());

        constants.sourceSize[0] = static_cast<float>(shownWidth);
        constants.sourceSize[1] = static_cast<float>(shownHeight);
        constants.origin[0] = onScreen.TopLeftX;
        constants.origin[1] = onScreen.TopLeftY;
        constants.sharpness = sharpness;
        context_->UpdateSubresource(outputConstants_.Get(), 0, nullptr, &constants, 0, 0);
        context_->OMSetRenderTargets(1, &screenTarget, depth.Get());
        ID3D11ShaderResourceView* scaledView = scaled_.view.Get();
        pass(sharpenShader_.Get(), nullptr, onScreen, &scaledView, 1, outputConstants_.Get());
    } else {
        context_->UpdateSubresource(outputConstants_.Get(), 0, nullptr, &constants, 0, 0);
        context_->OMSetRenderTargets(1, &screenTarget, depth.Get());
        pass(scaler, nullptr, onScreen, &picture, 1, outputConstants_.Get());
    }
}

} // namespace veloplay::render
