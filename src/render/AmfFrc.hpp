#pragma once

#include <memory>
#include <string>

#include <d3d11.h>
#include <wrl/client.h>

#include "render/FrameGenerator.hpp"

namespace veloplay::render {

/// AMD's frame rate conversion (AMF FRC) for the picture in the middle, on
/// AMD cards. AMF's runtime comes with AMD's driver (amfrt64.dll); it is
/// loaded when asked for, never linked. FRC always computes exactly the
/// middle between two pictures, so 3x and 4x, and anything FRC refuses, go
/// to the method it wraps. Beta: nobody has tried it on an AMD card yet.
class AmfFrcGenerator final : public IFrameGenerator {
public:
    /// Takes over `inner` for everything FRC does not do. Gives it back through release().
    explicit AmfFrcGenerator(std::unique_ptr<IFrameGenerator> inner);
    ~AmfFrcGenerator() override;

    /// Loads AMF and creates the FRC component on `device`. False with the reason when this PC cannot.
    bool create(ID3D11Device* device, ID3D11DeviceContext* context, std::string& error);
    /// The wrapped method, for going back to it.
    std::unique_ptr<IFrameGenerator> release();
    /// Why FRC stopped during use; empty while it runs.
    [[nodiscard]] const std::string& failure() const { return failure_; }

    [[nodiscard]] const char* name() const override { return failure_.empty() ? "AMD FRC" : inner_->name(); }
    [[nodiscard]] bool usesHardwareMotion() const override { return failure_.empty() || inner_->usesHardwareMotion(); }
    void allowHardwareMotion(bool allowed) override { inner_->allowHardwareMotion(allowed); }
    void holdStillParts(bool hold) override { inner_->holdStillParts(hold); }
    [[nodiscard]] std::string hardwareMotionError() const override {
        return failure_.empty() ? std::string{} : "AMD FRC: " + failure_;
    }
    bool prepare(int slot, ID3D11ShaderResourceView* picture, int width, int height) override;
    bool generate(int slotFrom, ID3D11ShaderResourceView* from, int slotTo, ID3D11ShaderResourceView* to, float phase,
                  ID3D11RenderTargetView* target) override;

private:
    struct Amf; // AMF's objects; its headers stay out of this one
    void fail(std::string why);

    std::unique_ptr<IFrameGenerator> inner_;
    std::unique_ptr<Amf> amf_;
    ID3D11DeviceContext* context_ = nullptr;
    std::string failure_;
    int width_ = 0;
    int height_ = 0;
    bool initialised_ = false;
    // The picture in the middle that FRC handed back, and the pair it belongs to.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> middle_;
    int middleFrom_ = -1;
    int middleTo_ = -1;
    int lastSlot_ = -1; // the slot of the picture FRC saw last
    // Each picture goes in with a time stamp of its own; the picture in between is only taken when its stamp
    // says which pair it belongs to. FRC might hand it back a call later, and then it belongs to the pair before.
    long long sequence_ = 0;
    long long previousStamp_ = -1;
};

} // namespace veloplay::render
