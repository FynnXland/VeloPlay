#pragma once

#include <optional>

#include <d3d11.h>
#include <wrl/client.h>

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

} // namespace veloplay::render
