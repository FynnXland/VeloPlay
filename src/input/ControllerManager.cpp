#include "input/ControllerManager.hpp"

#include "input/DualSenseEffects.hpp"

#include <algorithm>
#include <utility>

#include "input/InputMerger.hpp"

namespace veloplay::input {

int controllerRank(const ControllerInfo& info) {
    int rank = 0;
    switch (info.type) {
        case ControllerType::DualSense:
        case ControllerType::DualSenseEdge: rank = 40; break;
        case ControllerType::DualShock4: rank = 30; break;
        case ControllerType::Xbox: rank = 20; break;
        case ControllerType::Other: rank = 10; break;
        case ControllerType::Unknown: rank = 0; break;
    }
    // A cable gives the lowest input latency and the full feature set.
    if (info.connection == ConnectionType::Usb) rank += 5;
    return rank;
}

ControllerManager::ControllerManager(std::unique_ptr<IControllerBackend> backend) : backend_(std::move(backend)) {}

void ControllerManager::setPreferredName(std::string name) {
    if (name == preferredName_) return;
    preferredName_ = std::move(name);
    preferenceChanged_ = true;
}

void ControllerManager::update() {
    if (!backend_) return;
    backend_->poll();

    std::vector<ControllerInfo> current = backend_->controllers();
    for (const ControllerInfo& info : current) {
        const bool known = std::any_of(controllers_.begin(), controllers_.end(),
                                       [&](const ControllerInfo& old) { return old.id == info.id; });
        if (!known) events_.push_back({Event::Kind::Connected, info});
    }
    for (const ControllerInfo& old : controllers_) {
        const bool still = std::any_of(current.begin(), current.end(),
                                       [&](const ControllerInfo& info) { return info.id == old.id; });
        if (!still) events_.push_back({Event::Kind::Disconnected, old});
    }
    controllers_ = std::move(current);

    selectActive();

    state_ = {};
    if (activeId_) {
        if (const auto raw = backend_->read(*activeId_)) state_ = applyDeadzoneToSticks(*raw, deadzone_);
    }
}

void ControllerManager::selectActive() {
    const auto byId = [&](std::uint32_t id) {
        return std::find_if(controllers_.begin(), controllers_.end(),
                            [&](const ControllerInfo& info) { return info.id == id; });
    };

    // Keep the current controller while it is connected. Switching in the
    // middle of a game because a second pad showed up would be a nasty surprise.
    const bool activeConnected = activeId_ && byId(*activeId_) != controllers_.end();
    if (activeConnected && !preferenceChanged_) return;
    preferenceChanged_ = false;

    if (!preferredName_.empty()) {
        const auto preferred = std::find_if(controllers_.begin(), controllers_.end(), [&](const ControllerInfo& info) {
            return info.name == preferredName_;
        });
        if (preferred != controllers_.end()) {
            activeId_ = preferred->id;
            return;
        }
    }
    if (activeConnected) return;

    const auto best = std::max_element(controllers_.begin(), controllers_.end(),
                                       [](const ControllerInfo& a, const ControllerInfo& b) {
                                           return controllerRank(a) < controllerRank(b);
                                       });
    if (best == controllers_.end()) activeId_.reset();
    else activeId_ = best->id;
}

std::optional<ControllerInfo> ControllerManager::active() const {
    if (!activeId_) return std::nullopt;
    for (const ControllerInfo& info : controllers_) {
        if (info.id == *activeId_) return info;
    }
    return std::nullopt;
}

bool ControllerManager::rumble(std::uint16_t low, std::uint16_t high, std::chrono::milliseconds duration) {
    if (!backend_ || !activeId_) return false;
    return backend_->rumble(*activeId_, low, high, duration);
}

bool ControllerManager::triggerEffects(std::uint8_t typeLeft, std::span<const std::uint8_t> left,
                                       std::uint8_t typeRight, std::span<const std::uint8_t> right) {
    const auto info = active();
    if (!backend_ || !info) return false;
    if (info->type != ControllerType::DualSense && info->type != ControllerType::DualSenseEdge) return false;
    const auto block = buildTriggerEffect(typeLeft, left, typeRight, right);
    return backend_->sendEffect(info->id, block);
}

bool ControllerManager::clearTriggerEffects() {
    const std::array<std::uint8_t, kTriggerEffectParameters> none{};
    return triggerEffects(kTriggerEffectOff, none, kTriggerEffectOff, none);
}

std::vector<ControllerManager::Event> ControllerManager::takeEvents() {
    return std::exchange(events_, {});
}

} // namespace veloplay::input
