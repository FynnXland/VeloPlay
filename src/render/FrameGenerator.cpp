#include "render/FrameGenerator.hpp"

#include "render/NvofMotion.hpp"

#include <algorithm>

#include <d3dcompiler.h>

namespace veloplay::render {

using Microsoft::WRL::ComPtr;

namespace {

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

// The same block for all three shaders, so one buffer serves them.
#define VELOPLAY_FLOW_CONSTANTS                                                                                        \
    "cbuffer Flow : register(b0) {\n"                                                                                  \
    "    float2 lumaSize;      // size of the brightness level that is searched, in its pixels\n"                      \
    "    float scale;          // picture pixels per pixel of that level\n"                                            \
    "    float stepSize;       // distance between two shifts that are tried, in picture pixels\n"                     \
    "    float phase;          // where between the two pictures the new one lies, 0..1\n"                             \
    "    float mip;            // which copy of the brightness texture to read\n"                                      \
    "    float radius;         // shifts are tried from -radius to +radius steps\n"                                    \
    "    float hasPrediction;  // 1 if the level above has a result to start from\n"                                   \
    "    float2 frameSize;     // size of the pictures in pixels\n"                                                    \
    "    float2 cells;         // size of the texture of vectors a pass reads, where it reads one\n"                                                                                             \
    "};\n"

// Brightness of the picture at half its size. Reading between four pixels
// with the linear sampler averages them.
const char kLumaShader[] = R"(
Texture2D picture : register(t0);
SamplerState linearSampler : register(s0);
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float y = dot(picture.SampleLevel(linearSampler, uv, 0).rgb, float3(0.299, 0.587, 0.114));
    return float4(y, y, y, 1.0);
}
)";

// One level of the search. For every block: which shift makes the two
// pictures agree best? The shift is applied around the moment in between:
// back by `phase` of it in the earlier picture, forward by the rest in the
// later one. Sixteen samples spread over eight by eight pixels are compared.
// A shift away from what the level above found has to be clearly better to
// win; that keeps flat areas, where everything matches, from wandering.
const char kFlowShader[] = VELOPLAY_FLOW_CONSTANTS R"(
Texture2D lumaA : register(t0);
Texture2D lumaB : register(t1);
Texture2D coarse : register(t2);
SamplerState linearSampler : register(s0);

float cost(float2 centre, float2 shift) {
    float2 back = -phase * shift / scale;
    float2 forward = (1.0 - phase) * shift / scale;
    float sum = 0.0;
    [loop] for (int j = 0; j < 4; ++j) {
        [loop] for (int i = 0; i < 4; ++i) {
            float2 at = centre + (float2(i, j) - 1.5) * 2.0;
            float a = lumaA.SampleLevel(linearSampler, (at + back) / lumaSize, mip).r;
            float b = lumaB.SampleLevel(linearSampler, (at + forward) / lumaSize, mip).r;
            sum += abs(a - b);
        }
    }
    return sum / 16.0;
}

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 predicted = float2(0.0, 0.0);
    if (hasPrediction > 0.5) predicted = coarse.SampleLevel(linearSampler, uv, 0).xy;
    float2 centre = uv * lumaSize;

    // Standing still is always tried: a scoreboard over a moving background
    // must not be dragged along.
    float2 best = float2(0.0, 0.0);
    float bestCost = cost(centre, best) * 1.02 + 0.00002;

    int reach = int(radius + 0.5);
    [loop] for (int dy = -reach; dy <= reach; ++dy) {
        [loop] for (int dx = -reach; dx <= reach; ++dx) {
            float2 shift = predicted + float2(dx, dy) * stepSize;
            float penalty = 0.04 * (abs(dx) + abs(dy));
            float c = cost(centre, shift) * (1.0 + penalty) + penalty * 0.001;
            if (c < bestCost) {
                bestCost = c;
                best = shift;
            }
        }
    }
    return float4(best, bestCost, 1.0);
}
)";

// The card's motion search says where each part of the earlier picture is
// in the later one. The new picture needs it the other way round: for a
// place in the new picture, which part arrives there? Looked up in three
// steps: take the vector at the place, go back along it by the share of
// the way already done, and take the vector found there.
const char kCarryShader[] = VELOPLAY_FLOW_CONSTANTS R"(
Texture2D<int2> vectors : register(t0);

// One vector per cell of `scale` by `scale` picture pixels, in 1/32 pixel.
float2 vectorAt(float2 pixel) {
    int2 cell = clamp(int2(floor(pixel / scale)), int2(0, 0), int2(lumaSize) - 1);
    return float2(vectors.Load(int3(cell, 0))) / 32.0;
}

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 here = uv * frameSize;
    float2 v = vectorAt(here);
    v = vectorAt(here - phase * v);
    v = vectorAt(here - phase * v);
    return float4(v, 0.0, 1.0);
}
)";

// At the edge of something that moves, the vector carried over to a place
// can belong to the other side of the edge. So every place tries the
// vectors of the cells around it as well, and standing still, and keeps the
// one with which the two pictures agree best there. Compared as in the
// built-in search: sixteen samples of brightness around the moment in between.
const char kChooseShader[] = VELOPLAY_FLOW_CONSTANTS R"(
Texture2D lumaA : register(t0);
Texture2D lumaB : register(t1);
Texture2D carried : register(t2);
SamplerState linearSampler : register(s0);
SamplerState pointSampler : register(s1);

float cost(float2 centre, float2 shift) {
    float2 back = -phase * shift / scale;
    float2 forward = (1.0 - phase) * shift / scale;
    float sum = 0.0;
    [loop] for (int j = 0; j < 4; ++j) {
        [loop] for (int i = 0; i < 4; ++i) {
            float2 at = centre + (float2(i, j) - 1.5) * 2.0;
            float a = lumaA.SampleLevel(linearSampler, (at + back) / lumaSize, mip).r;
            float b = lumaB.SampleLevel(linearSampler, (at + forward) / lumaSize, mip).r;
            sum += abs(a - b);
        }
    }
    return sum / 16.0;
}

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 centre = uv * lumaSize;
    // Standing still is always tried: a scoreboard over a moving background must not be dragged along.
    float2 best = float2(0.0, 0.0);
    float bestCost = cost(centre, best) * 1.02 + 0.00002;
    [loop] for (int dy = -1; dy <= 1; ++dy) {
        [loop] for (int dx = -1; dx <= 1; ++dx) {
            float2 shift = carried.SampleLevel(pointSampler, uv + float2(dx, dy) / cells, 0).xy;
            // The place's own vector wins where the others are no better.
            float c = cost(centre, shift) * (1.0 + 0.02 * (abs(dx) + abs(dy)));
            if (c < bestCost) {
                bestCost = c;
                best = shift;
            }
        }
    }
    return float4(best, bestCost, 1.0);
}
)";

// The new picture. Every pixel is fetched from both pictures along the
// shift found for its place and mixed by how close the moment is to each.
// Where the two fetched colours differ a lot, the shift cannot be trusted
// there (something appeared or disappeared, or the search went wrong); the
// pixel then turns into a plain mix of the two pictures at that place.
const char kSynthesisShader[] = VELOPLAY_FLOW_CONSTANTS R"(
Texture2D frameA : register(t0);
Texture2D frameB : register(t1);
Texture2D flow : register(t2);
SamplerState linearSampler : register(s0);
SamplerState pointSampler : register(s1);

// A fetch between pixels with the linear filter blurs a little, and the new
// picture is made of two such fetches. Next to the real pictures, which are
// not blurred, that shows as a flicker in sharpness sixty times a second.
// A cubic filter with a strong negative lobe (Keys, a = -1) keeps the new
// picture about as crisp as the ones around it.
float cubicWeight(float d) {
    d = abs(d);
    if (d <= 1.0) return d * d * d - 2.0 * d * d + 1.0;
    if (d < 2.0) return -d * d * d + 5.0 * d * d - 8.0 * d + 4.0;
    return 0.0;
}
float3 fetch(Texture2D picture, float2 at) {
    float2 p = at * frameSize - 0.5;
    float2 base = floor(p);
    float2 f = p - base;
    float3 sum = float3(0.0, 0.0, 0.0);
    float total = 0.0;
    [unroll] for (int j = -1; j <= 2; ++j) {
        [unroll] for (int i = -1; i <= 2; ++i) {
            float w = cubicWeight(float(i) - f.x) * cubicWeight(float(j) - f.y);
            sum += picture.SampleLevel(pointSampler, (base + float2(i, j) + 0.5) / frameSize, 0).rgb * w;
            total += w;
        }
    }
    return saturate(sum / total);
}

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 shift = flow.SampleLevel(linearSampler, uv, 0).xy / frameSize;
    float3 a = fetch(frameA, uv - phase * shift);
    float3 b = fetch(frameB, uv + (1.0 - phase) * shift);
    float difference = dot(abs(a - b), float3(0.299, 0.587, 0.114));
    float doubt = smoothstep(0.10, 0.30, difference);
    float3 plain = lerp(frameA.SampleLevel(linearSampler, uv, 0).rgb, frameB.SampleLevel(linearSampler, uv, 0).rgb, phase);
    return float4(lerp(lerp(a, b, phase), plain, doubt), 1.0);
}
)";

struct Constants {
    float lumaSize[2];
    float scale;
    float stepSize;
    float phase;
    float mip;
    float radius;
    float hasPrediction;
    float frameSize[2];
    float cells[2];
};

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

constexpr int kMaxLevels = 6;

} // namespace

FlowFrameGenerator::FlowFrameGenerator() = default;
FlowFrameGenerator::~FlowFrameGenerator() = default;

void FlowFrameGenerator::dropHardwareMotion(const std::string& reason) {
    nvofError_ = reason.empty() ? std::string("the optical flow engine failed") : reason;
    nvof_.reset();
    nvofVectors_ = nullptr;
    nvofFrom_ = -1;
    nvofTo_ = -1;
    nvofInput_[0] = false;
    nvofInput_[1] = false;
}

bool FlowFrameGenerator::create(ID3D11Device* device, ID3D11DeviceContext* context, std::string& error) {
    device_ = device;
    context_ = context;

    ComPtr<ID3DBlob> vertexCode;
    if (!compile(kVertexShader, sizeof(kVertexShader) - 1, "vs_4_0", vertexCode, error)) return false;
    if (FAILED(device_->CreateVertexShader(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr,
                                           vertexShader_.GetAddressOf()))) {
        error = "vertex shader could not be created";
        return false;
    }
    if (!makePixelShader(device, kLumaShader, sizeof(kLumaShader) - 1, lumaShader_, error) ||
        !makePixelShader(device, kFlowShader, sizeof(kFlowShader) - 1, flowShader_, error) ||
        !makePixelShader(device, kSynthesisShader, sizeof(kSynthesisShader) - 1, synthesisShader_, error)) {
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
    if (FAILED(device_->CreateSamplerState(&sampler, sampler_.GetAddressOf())) ||
        FAILED(device_->CreateSamplerState(&point, pointSampler_.GetAddressOf())) ||
        FAILED(device_->CreateRasterizerState(&rasterizer, rasterizer_.GetAddressOf())) ||
        FAILED(device_->CreateBuffer(&buffer, nullptr, constants_.GetAddressOf()))) {
        error = "render states could not be created";
        return false;
    }

    // The card's own motion search is an extra on top. Without it, or
    // without the two shaders that use its result, the built-in search does the work.
    // The shaders come first, on every card: a mistake in them then shows
    // on the machines that build and test, which have no NVIDIA card.
    if (!makePixelShader(device, kCarryShader, sizeof(kCarryShader) - 1, projectShader_, nvofError_) ||
        !makePixelShader(device, kChooseShader, sizeof(kChooseShader) - 1, chooseShader_, nvofError_)) {
        return true;
    }
    auto nvof = std::make_unique<NvofMotion>();
    if (nvof->create(device, context, nvofError_)) nvof_ = std::move(nvof);
    return true;
}

bool FlowFrameGenerator::makeFlow(Level& level) {
    return makeVectors(level, std::max(1, (level.lumaWidth + 3) / 4), std::max(1, (level.lumaHeight + 3) / 4));
}

bool FlowFrameGenerator::makeVectors(Level& level, int width, int height) {
    level.flowWidth = width;
    level.flowHeight = height;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(level.flowWidth);
    desc.Height = static_cast<UINT>(level.flowHeight);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    // Shifts in picture pixels, with sign and fractions; the third channel keeps how well it matched.
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    return SUCCEEDED(device_->CreateTexture2D(&desc, nullptr, level.flow.ReleaseAndGetAddressOf())) &&
           SUCCEEDED(device_->CreateShaderResourceView(level.flow.Get(), nullptr,
                                                       level.flowView.ReleaseAndGetAddressOf())) &&
           SUCCEEDED(device_->CreateRenderTargetView(level.flow.Get(), nullptr,
                                                     level.flowTarget.ReleaseAndGetAddressOf()));
}

bool FlowFrameGenerator::ensureSize(int width, int height) {
    if (width == width_ && height == height_ && !levels_.empty()) return true;
    width_ = 0;
    height_ = 0;
    levels_.clear();
    // Pictures of the old size are of no use to the card's search either.
    nvofInput_[0] = false;
    nvofInput_[1] = false;
    nvofVectors_ = nullptr;
    nvofFrom_ = -1;
    nvofTo_ = -1;

    const int lumaWidth = std::max(1, width / 2);
    const int lumaHeight = std::max(1, height / 2);
    // Levels go down until the smaller side would drop below eight pixels.
    int count = 1;
    while (count < kMaxLevels && std::min(lumaWidth >> (count - 1), lumaHeight >> (count - 1)) >= 16) ++count;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(lumaWidth);
    desc.Height = static_cast<UINT>(lumaHeight);
    desc.MipLevels = static_cast<UINT>(count);
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // every card can build the smaller copies of this format itself
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    D3D11_RENDER_TARGET_VIEW_DESC target{};
    target.Format = desc.Format;
    target.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    for (int slot = 0; slot < 2; ++slot) {
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, luma_[slot].ReleaseAndGetAddressOf())) ||
            FAILED(device_->CreateShaderResourceView(luma_[slot].Get(), nullptr,
                                                     lumaView_[slot].ReleaseAndGetAddressOf())) ||
            FAILED(device_->CreateRenderTargetView(luma_[slot].Get(), &target,
                                                   lumaTarget_[slot].ReleaseAndGetAddressOf()))) {
            return false;
        }
    }

    levels_.resize(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        Level& level = levels_[static_cast<std::size_t>(index)];
        level.lumaWidth = std::max(1, lumaWidth >> index);
        level.lumaHeight = std::max(1, lumaHeight >> index);
        if (!makeFlow(level)) {
            levels_.clear();
            return false;
        }
    }
    refined_ = Level{};
    refined_.lumaWidth = lumaWidth;
    refined_.lumaHeight = lumaHeight;
    if (!makeFlow(refined_)) {
        levels_.clear();
        return false;
    }
    width_ = width;
    height_ = height;
    return true;
}

void FlowFrameGenerator::pass(ID3D11PixelShader* shader, ID3D11RenderTargetView* target, int width, int height,
                              ID3D11ShaderResourceView* const* views, UINT viewCount) {
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;
    ID3D11SamplerState* samplers[2] = {sampler_.Get(), pointSampler_.Get()};
    ID3D11Buffer* buffers[1] = {constants_.Get()};
    const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    context_->OMSetRenderTargets(1, &target, nullptr);
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

    // Nothing stays bound: the next pass writes to what this one read.
    ID3D11ShaderResourceView* none[3] = {nullptr, nullptr, nullptr};
    context_->PSSetShaderResources(0, 3, none);
    ID3D11RenderTargetView* noTarget = nullptr;
    context_->OMSetRenderTargets(1, &noTarget, nullptr);
}

bool FlowFrameGenerator::prepare(int slot, ID3D11ShaderResourceView* picture, int width, int height) {
    if (slot < 0 || slot > 1 || !picture || !ensureSize(width, height)) return false;
    ID3D11ShaderResourceView* views[1] = {picture};
    pass(lumaShader_.Get(), lumaTarget_[slot].Get(), levels_[0].lumaWidth, levels_[0].lumaHeight, views, 1);
    context_->GenerateMips(lumaView_[slot].Get());

    if (usesHardwareMotion()) {
        // The card's search gets the picture as well, as brightness at full size.
        if (ID3D11RenderTargetView* input = nvof_->input(slot, width, height)) {
            pass(lumaShader_.Get(), input, width, height, views, 1);
            nvofInput_[slot] = true;
            nvofVectors_ = nullptr; // what was found belongs to the pair before
        } else {
            dropHardwareMotion(nvof_->error());
        }
    }
    return true;
}

bool FlowFrameGenerator::hardwareFlow(int slotFrom, int slotTo, float phase, ID3D11ShaderResourceView*& flow) {
    if (!nvofInput_[slotFrom] || !nvofInput_[slotTo]) return false;
    // The search runs once per pair of pictures, however many pictures are computed between the two.
    if (!nvofVectors_ || nvofFrom_ != slotFrom || nvofTo_ != slotTo) {
        nvofVectors_ = nvof_->motion(slotFrom, slotTo);
        if (!nvofVectors_) {
            dropHardwareMotion(nvof_->error());
            return false;
        }
        nvofFrom_ = slotFrom;
        nvofTo_ = slotTo;
    }
    const int cellsWide = nvof_->cellsWide();
    const int cellsHigh = nvof_->cellsHigh();
    if (carried_.flowWidth != cellsWide || carried_.flowHeight != cellsHigh || !carried_.flow || !chosen_.flow) {
        if (!makeVectors(carried_, cellsWide, cellsHigh) || !makeVectors(chosen_, cellsWide, cellsHigh)) {
            dropHardwareMotion("the textures for the card's motion vectors could not be created");
            return false;
        }
    }

    Constants constants{};
    constants.phase = phase;
    constants.frameSize[0] = static_cast<float>(width_);
    constants.frameSize[1] = static_cast<float>(height_);
    constants.cells[0] = static_cast<float>(cellsWide);
    constants.cells[1] = static_cast<float>(cellsHigh);

    // To the moment in between.
    constants.lumaSize[0] = static_cast<float>(cellsWide);
    constants.lumaSize[1] = static_cast<float>(cellsHigh);
    constants.scale = static_cast<float>(nvof_->grid());
    context_->UpdateSubresource(constants_.Get(), 0, nullptr, &constants, 0, 0);
    {
        ID3D11ShaderResourceView* views[1] = {nvofVectors_};
        pass(projectShader_.Get(), carried_.flowTarget.Get(), cellsWide, cellsHigh, views, 1);
    }
    // Per place the best of the vectors around it, judged on the brightness at half size.
    constants.lumaSize[0] = static_cast<float>(levels_[0].lumaWidth);
    constants.lumaSize[1] = static_cast<float>(levels_[0].lumaHeight);
    constants.scale = 2.0f;
    constants.mip = 0.0f;
    context_->UpdateSubresource(constants_.Get(), 0, nullptr, &constants, 0, 0);
    {
        ID3D11ShaderResourceView* views[3] = {lumaView_[slotFrom].Get(), lumaView_[slotTo].Get(), carried_.flowView.Get()};
        pass(chooseShader_.Get(), chosen_.flowTarget.Get(), cellsWide, cellsHigh, views, 3);
    }
    flow = chosen_.flowView.Get();
    return true;
}

bool FlowFrameGenerator::generate(int slotFrom, ID3D11ShaderResourceView* from, int slotTo, ID3D11ShaderResourceView* to,
                                  float phase, ID3D11RenderTargetView* target) {
    if (levels_.empty() || slotFrom < 0 || slotFrom > 1 || slotTo < 0 || slotTo > 1 || !from || !to || !target) {
        return false;
    }
    Constants constants{};
    constants.phase = std::clamp(phase, 0.0f, 1.0f);
    constants.frameSize[0] = static_cast<float>(width_);
    constants.frameSize[1] = static_cast<float>(height_);

    if (usesHardwareMotion()) {
        ID3D11ShaderResourceView* flow = nullptr;
        if (hardwareFlow(slotFrom, slotTo, constants.phase, flow)) {
            ID3D11ShaderResourceView* views[3] = {from, to, flow};
            pass(synthesisShader_.Get(), target, width_, height_, views, 3);
            return true;
        }
        // Not this time, or never again: the built-in search below does it.
    }

    // From the smallest copies to the largest, each level starting from the one above.
    ID3D11ShaderResourceView* previous = nullptr;
    for (int index = static_cast<int>(levels_.size()) - 1; index >= 0; --index) {
        const Level& level = levels_[static_cast<std::size_t>(index)];
        constants.lumaSize[0] = static_cast<float>(level.lumaWidth);
        constants.lumaSize[1] = static_cast<float>(level.lumaHeight);
        constants.scale = static_cast<float>(2 << index); // half size, halved again per level
        constants.stepSize = constants.scale;
        constants.mip = static_cast<float>(index);
        constants.radius = 2.0f;
        constants.hasPrediction = previous ? 1.0f : 0.0f;
        context_->UpdateSubresource(constants_.Get(), 0, nullptr, &constants, 0, 0);
        ID3D11ShaderResourceView* views[3] = {lumaView_[slotFrom].Get(), lumaView_[slotTo].Get(), previous};
        pass(flowShader_.Get(), level.flowTarget.Get(), level.flowWidth, level.flowHeight, views, 3);
        previous = level.flowView.Get();
    }
    // Once more on the finest level with half steps, which is one picture pixel.
    constants.stepSize = constants.scale * 0.5f;
    constants.radius = 1.0f;
    constants.hasPrediction = 1.0f;
    context_->UpdateSubresource(constants_.Get(), 0, nullptr, &constants, 0, 0);
    {
        ID3D11ShaderResourceView* views[3] = {lumaView_[slotFrom].Get(), lumaView_[slotTo].Get(), previous};
        pass(flowShader_.Get(), refined_.flowTarget.Get(), refined_.flowWidth, refined_.flowHeight, views, 3);
    }

    ID3D11ShaderResourceView* views[3] = {from, to, refined_.flowView.Get()};
    pass(synthesisShader_.Get(), target, width_, height_, views, 3);
    return true;
}

} // namespace veloplay::render
