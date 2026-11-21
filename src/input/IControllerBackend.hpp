#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "input/ControllerState.hpp"

namespace veloplay::input {

enum class ControllerType { Unknown, DualSense, DualSenseEdge, DualShock4, Xbox, Other };
enum class ConnectionType { Unknown, Usb, Bluetooth };

struct ControllerInfo {
    std::uint32_t id = 0; // stable while the controller stays connected
    std::string name;
    ControllerType type = ControllerType::Unknown;
    ConnectionType connection = ConnectionType::Unknown;
    bool hasTouchpad = false;
    bool hasMotion = false;
    bool hasRumble = false;
    /// Charge of the controller's battery in percent; -1 if it has none or does not say.
    int batteryPercent = -1;
    bool charging = false; // on the cable and charging, or full

    friend bool operator==(const ControllerInfo&, const ControllerInfo&) = default;
};

/// Access to physical game controllers. The client ships an SDL3 backend;
/// tests use a scripted one.
class IControllerBackend {
public:
    virtual ~IControllerBackend() = default;

    /// Processes pending device events. Call once per frame on the thread that
    /// created the backend.
    virtual void poll() = 0;

    /// Fetches what the devices report right now, so that read() returns
    /// fresh values between two calls of poll(). Together with read() this
    /// may be called from any thread, also while another one is in poll().
    virtual void refresh() {}

    [[nodiscard]] virtual std::vector<ControllerInfo> controllers() const = 0;

    /// Raw state without dead zone. Empty if the controller is gone.
    [[nodiscard]] virtual std::optional<ControllerState> read(std::uint32_t id) = 0;

    /// Classic two-motor rumble. Both strengths are 0..65535.
    virtual bool rumble(std::uint32_t id, std::uint16_t low, std::uint16_t high, std::chrono::milliseconds duration) = 0;

    /// Sends a controller-specific effects block, see DualSenseEffects.hpp.
    /// Returns false if the controller does not take such blocks.
    virtual bool sendEffect(std::uint32_t id, std::span<const std::uint8_t> block) = 0;
};

} // namespace veloplay::input
