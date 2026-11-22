#include "render/FrameGrabber.hpp"

#include <algorithm>
#include <cstring>

namespace veloplay::render {

void FrameGrabber::capture(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source,
                           const D3D11_BOX& region) {
    if (!wanted_ || pending_ || !device || !context || !source) return;
    wanted_ = false;
    failure_ = "";
    D3D11_TEXTURE2D_DESC from{};
    source->GetDesc(&from);
    if (from.Format != DXGI_FORMAT_B8G8R8A8_UNORM && from.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        failure_ = "the screen's picture is in a format that cannot be saved";
        return;
    }
    D3D11_BOX box = region;
    box.right = std::min<UINT>(box.right, from.Width);
    box.bottom = std::min<UINT>(box.bottom, from.Height);
    box.front = 0;
    box.back = 1;
    if (box.right <= box.left || box.bottom <= box.top) {
        failure_ = "there is no picture on screen";
        return;
    }
    const int width = static_cast<int>(box.right - box.left);
    const int height = static_cast<int>(box.bottom - box.top);
    if (!staging_ || width != width_ || height != height_ || (from.Format == DXGI_FORMAT_R8G8B8A8_UNORM) != swapRedBlue_) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = from.Format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging_.Reset();
        if (FAILED(device->CreateTexture2D(&desc, nullptr, staging_.GetAddressOf()))) {
            failure_ = "no memory for a copy of the picture";
            return;
        }
        width_ = width;
        height_ = height;
        swapRedBlue_ = from.Format == DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    context->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, source, 0, &box);
    pending_ = true;
    waited_ = 0;
}

std::optional<media::Image> FrameGrabber::take(ID3D11DeviceContext* context) {
    if (!pending_ || !context) return std::nullopt;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT result = context->Map(staging_.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (result == DXGI_ERROR_WAS_STILL_DRAWING) {
        // A card that has not got there after a few hundred frames will not; give the request up.
        if (++waited_ > 600) {
            pending_ = false;
            failure_ = "the graphics card did not hand the picture over";
        }
        return std::nullopt;
    }
    pending_ = false;
    if (FAILED(result)) {
        failure_ = "the copy of the picture could not be read";
        return std::nullopt;
    }
    media::Image image;
    image.width = width_;
    image.height = height_;
    image.bgra.resize(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 4);
    const std::size_t row = static_cast<std::size_t>(width_) * 4;
    for (int y = 0; y < height_; ++y) {
        std::memcpy(image.bgra.data() + static_cast<std::size_t>(y) * row,
                    static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch, row);
    }
    context->Unmap(staging_.Get(), 0);
    if (swapRedBlue_) {
        for (std::size_t at = 0; at < image.bgra.size(); at += 4) std::swap(image.bgra[at], image.bgra[at + 2]);
    }
    // The screen has no transparency; what the swap chain holds there is meaningless.
    for (std::size_t at = 3; at < image.bgra.size(); at += 4) image.bgra[at] = 255;
    return image;
}

} // namespace veloplay::render
