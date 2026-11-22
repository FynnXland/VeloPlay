#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "media/Recording.hpp"
#include "media/Snapshot.hpp"

namespace veloplay::render {

/// Takes a copy of the picture as it is on screen, after frame generation and
/// scaling, before any of VeloPlay's own overlays are drawn over it.
///
/// Never waits for the graphics card: capture() only queues a copy into a
/// texture the processor can read, and take() looks once per frame whether
/// that copy has arrived. A screenshot arrives one or two frames after the
/// key, and the window keeps its rhythm.
class FrameGrabber {
public:
    /// The next capture() takes a copy.
    void request() { wanted_ = true; }
    [[nodiscard]] bool busy() const { return wanted_ || pending_; }

    /// Call right after the picture was drawn into `source`. `region` is the
    /// part that shows the picture, without the black bars. Does nothing
    /// unless a copy was requested.
    void capture(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source, const D3D11_BOX& region);

    /// The picture once its copy is there; nothing while it is on its way.
    std::optional<media::Image> take(ID3D11DeviceContext* context);

    /// Why the last request came to nothing, empty if it did not.
    [[nodiscard]] const char* failure() const { return failure_; }

private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    bool wanted_ = false;
    bool pending_ = false;
    bool swapRedBlue_ = false;
    int width_ = 0;
    int height_ = 0;
    int waited_ = 0;
    const char* failure_ = "";
};

/// The same for a recording: a copy of every picture, in order. The copies
/// go through a ring of textures the processor can read, so the card has a
/// few frames to hand each one over and the window never waits for it.
class FrameStream {
public:
    struct Picture {
        std::vector<std::uint8_t> bgra; // rows top to bottom, four bytes per pixel
        media::ShownKind kind = media::ShownKind::Real;
        std::chrono::steady_clock::time_point at;
    };

    /// Queues a copy of `region` of `source`, `width` x `height` from its top left. False if
    /// the ring is full (the copies before have not arrived yet), so this picture is left out.
    bool capture(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source, UINT left, UINT top,
                 int width, int height, media::ShownKind kind, std::chrono::steady_clock::time_point at);
    /// The oldest copy once it has arrived. `reuse` is memory for it, may be empty.
    std::optional<Picture> take(ID3D11DeviceContext* context, std::vector<std::uint8_t>&& reuse);
    /// Forgets the copies on their way and the textures.
    void reset();
    [[nodiscard]] const char* failure() const { return failure_; }

private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        bool pending = false;
        media::ShownKind kind = media::ShownKind::Real;
        std::chrono::steady_clock::time_point at;
    };
    std::array<Slot, 4> slots_;
    std::size_t next_ = 0;   // where the next copy goes
    std::size_t oldest_ = 0; // the copy to hand over next
    int width_ = 0;
    int height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    const char* failure_ = "";
};

} // namespace veloplay::render
