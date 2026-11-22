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

bool FrameStream::capture(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source, UINT left,
                          UINT top, int width, int height, media::ShownKind kind,
                          std::chrono::steady_clock::time_point at) {
    if (!device || !context || !source || width <= 0 || height <= 0) return false;
    D3D11_TEXTURE2D_DESC from{};
    source->GetDesc(&from);
    if (from.Format != DXGI_FORMAT_B8G8R8A8_UNORM && from.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        failure_ = "the screen's picture is in a format that cannot be recorded";
        return false;
    }
    if (left + static_cast<UINT>(width) > from.Width || top + static_cast<UINT>(height) > from.Height) return false;
    if (width != width_ || height != height_ || from.Format != format_) {
        reset();
        width_ = width;
        height_ = height;
        format_ = from.Format;
    }
    Slot& slot = slots_[next_];
    if (slot.pending) return false;
    if (!slot.staging) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = from.Format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, slot.staging.GetAddressOf()))) {
            failure_ = "no memory for copies of the picture";
            return false;
        }
    }
    D3D11_BOX box{left, top, 0, left + static_cast<UINT>(width), top + static_cast<UINT>(height), 1};
    context->CopySubresourceRegion(slot.staging.Get(), 0, 0, 0, 0, source, 0, &box);
    slot.pending = true;
    slot.kind = kind;
    slot.at = at;
    next_ = (next_ + 1) % slots_.size();
    return true;
}

std::optional<FrameStream::Picture> FrameStream::take(ID3D11DeviceContext* context, std::vector<std::uint8_t>&& reuse) {
    Slot& slot = slots_[oldest_];
    if (!slot.pending || !context) return std::nullopt;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT result = context->Map(slot.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (result == DXGI_ERROR_WAS_STILL_DRAWING) return std::nullopt;
    slot.pending = false;
    oldest_ = (oldest_ + 1) % slots_.size();
    if (FAILED(result)) {
        failure_ = "a copy of the picture could not be read";
        return std::nullopt;
    }
    Picture picture;
    picture.kind = slot.kind;
    picture.at = slot.at;
    picture.bgra = std::move(reuse);
    const std::size_t row = static_cast<std::size_t>(width_) * 4;
    picture.bgra.resize(row * static_cast<std::size_t>(height_));
    for (int y = 0; y < height_; ++y) {
        std::memcpy(picture.bgra.data() + static_cast<std::size_t>(y) * row,
                    static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch, row);
    }
    context->Unmap(slot.staging.Get(), 0);
    if (format_ == DXGI_FORMAT_R8G8B8A8_UNORM) {
        for (std::size_t at = 0; at < picture.bgra.size(); at += 4) std::swap(picture.bgra[at], picture.bgra[at + 2]);
    }
    return picture;
}

void FrameStream::reset() {
    for (Slot& slot : slots_) slot = Slot{};
    next_ = 0;
    oldest_ = 0;
    width_ = 0;
    height_ = 0;
    format_ = DXGI_FORMAT_UNKNOWN;
}

} // namespace veloplay::render
