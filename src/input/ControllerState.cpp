#include "input/ControllerState.hpp"

#include <utility>

namespace veloplay::input {

namespace {

constexpr std::pair<Button, std::string_view> kButtonNames[] = {
    {Button::Cross, "cross"},        {Button::Circle, "circle"},       {Button::Square, "square"},
    {Button::Triangle, "triangle"},  {Button::DpadLeft, "dpad_left"},  {Button::DpadRight, "dpad_right"},
    {Button::DpadUp, "dpad_up"},     {Button::DpadDown, "dpad_down"},  {Button::L1, "l1"},
    {Button::R1, "r1"},              {Button::L3, "l3"},               {Button::R3, "r3"},
    {Button::Options, "options"},    {Button::Create, "create"},       {Button::Touchpad, "touchpad"},
    {Button::Ps, "ps"},              {Button::Mute, "mute"},
};

} // namespace

std::string_view buttonName(Button button) {
    for (const auto& [value, name] : kButtonNames) {
        if (value == button) return name;
    }
    return "unknown";
}

std::optional<Button> parseButton(std::string_view name) {
    for (const auto& [value, known] : kButtonNames) {
        if (known == name) return value;
    }
    return std::nullopt;
}

} // namespace veloplay::input
