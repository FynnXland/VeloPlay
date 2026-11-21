#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "input/IControllerBackend.hpp"

namespace veloplay::input {

/// Tracks connected controllers, picks the one that drives the session and
/// reports plug and unplug events. Works without a backend (no controllers).
class ControllerManager {
public:
    struct Event {
        enum class Kind { Connected, Disconnected };
        Kind kind;
        ControllerInfo info;
    };

    explicit ControllerManager(std::unique_ptr<IControllerBackend> backend);

    void setDeadzone(float deadzone) {
        deadzone_ = deadzone;
        liveDeadzone_.store(deadzone, std::memory_order_relaxed);
    }

    /// State of the active controller as the device reports it this moment,
    /// with the dead zone applied. Unlike state(), which is what update() saw
    /// last, this may be called from any thread and as often as wanted.
    [[nodiscard]] ControllerState readNow();

    /// Name of the controller the user prefers. Empty means automatic choice.
    void setPreferredName(std::string name);

    /// Call once per frame.
    void update();

    [[nodiscard]] const std::vector<ControllerInfo>& controllers() const { return controllers_; }
    [[nodiscard]] std::optional<ControllerInfo> active() const;

    /// State of the active controller with the dead zone applied. Neutral if
    /// no controller is connected.
    [[nodiscard]] const ControllerState& state() const { return state_; }

    bool rumble(std::uint16_t low, std::uint16_t high, std::chrono::milliseconds duration);

    /// Sets the adaptive triggers of the active controller. Does nothing and
    /// returns false unless it is a DualSense.
    bool triggerEffects(std::uint8_t typeLeft, std::span<const std::uint8_t> left, std::uint8_t typeRight,
                        std::span<const std::uint8_t> right);
    /// Releases both triggers.
    bool clearTriggerEffects();

    /// Plug and unplug events since the last call.
    std::vector<Event> takeEvents();

private:
    void selectActive();

    std::unique_ptr<IControllerBackend> backend_;
    std::vector<ControllerInfo> controllers_;
    std::optional<std::uint32_t> activeId_;
    std::string preferredName_;
    bool preferenceChanged_ = false;
    float deadzone_ = 0.08f;
    ControllerState state_{};
    std::vector<Event> events_;
    // What readNow() needs, readable from another thread. Id 0 is never a controller.
    std::atomic<std::uint32_t> liveActiveId_{0};
    std::atomic<bool> liveHasActive_{false};
    std::atomic<float> liveDeadzone_{0.08f};
};

/// How well a controller suits a PS5 session. Higher is better.
int controllerRank(const ControllerInfo& info);

} // namespace veloplay::input
