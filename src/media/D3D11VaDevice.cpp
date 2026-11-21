// Windows only: hands an existing Direct3D 11 device to FFmpeg so that decoded
// pictures are textures on the same device the renderer draws with.

#include <d3d11.h>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

namespace veloplay::media {

AVBufferRef* createD3D11VaDevice(void* d3d11Device) {
    auto* device = static_cast<ID3D11Device*>(d3d11Device);
    if (!device) return nullptr;

    AVBufferRef* reference = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!reference) return nullptr;
    auto* context = reinterpret_cast<AVHWDeviceContext*>(reference->data);
    auto* d3d = static_cast<AVD3D11VADeviceContext*>(context->hwctx);
    device->AddRef(); // released by FFmpeg together with the context
    d3d->device = device;

    // The decoder uses the immediate context from the session's thread while
    // the renderer uses it from the window's thread.
    ID3D10Multithread* multithread = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void**>(&multithread)))) {
        multithread->SetMultithreadProtected(TRUE);
        multithread->Release();
    }

    // Fails on devices without a video decoder, such as the software renderer.
    if (av_hwdevice_ctx_init(reference) < 0) {
        av_buffer_unref(&reference);
        return nullptr;
    }
    return reference;
}

} // namespace veloplay::media
