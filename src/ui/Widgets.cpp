#include "ui/Widgets.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <imgui_stdlib.h>

namespace veloplay::ui {

namespace {

constexpr float kTwoPi = 6.2831853f;

std::string_view visibleLabel(const char* label) {
    const char* hidden = std::strstr(label, "##");
    return hidden ? std::string_view(label, static_cast<std::size_t>(hidden - label)) : std::string_view(label);
}

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

} // namespace

// ---- Animator ----------------------------------------------------------

float Animator::to(ImGuiID id, float target, float response) { return to(id, target, response, target); }

float Animator::to(ImGuiID id, float target, float response, float initial) {
    Spring& spring = springs_.try_emplace(id, Spring{initial, 0.0f}).first->second;
    if (reducedMotion_) {
        spring = {target, 0.0f};
        return target;
    }

    // Exact solution of a critically damped spring for one time step.
    const float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
    const float omega = kTwoPi / std::max(response, 0.01f);
    const float offset = spring.value - target;
    const float c2 = spring.velocity + omega * offset;
    const float decay = std::exp(-omega * dt);
    spring.value = target + (offset + c2 * dt) * decay;
    spring.velocity = (c2 - omega * (offset + c2 * dt)) * decay;

    if (std::abs(spring.value - target) < 0.0005f && std::abs(spring.velocity) < 0.0005f) spring = {target, 0.0f};
    return spring.value;
}

Context& context() {
    static Context instance;
    return instance;
}

float px(float value) { return value * context().scale; }

void space(float height) { ImGui::Dummy(ImVec2(0.0f, px(height))); }

// ---- text ----------------------------------------------------------------

void pushText(TextStyle style) {
    const TextSpec spec = textSpec(style, context().fonts);
    ImGui::PushFont(spec.font, spec.size);
}

void popText() { ImGui::PopFont(); }

void label(TextStyle style, std::string_view text, const ImVec4& color) {
    pushText(style);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopStyleColor();
    popText();
}

void label(TextStyle style, std::string_view text) { label(style, text, palette().text); }

void paragraph(TextStyle style, std::string_view text, const ImVec4& color) {
    pushText(style);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    popText();
}

ImVec2 measure(TextStyle style, std::string_view text) {
    pushText(style);
    const ImVec2 size = ImGui::CalcTextSize(text.data(), text.data() + text.size());
    popText();
    return size;
}

void drawText(ImDrawList* draw, ImVec2 position, TextStyle style, const ImVec4& color, std::string_view text,
              float alpha) {
    pushText(style);
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), position, toU32(color, alpha), text.data(),
                  text.data() + text.size());
    popText();
}

// ---- button --------------------------------------------------------------

ImVec2 buttonSize(const char* label) {
    const ImVec2 text = measure(TextStyle::Strong, visibleLabel(label));
    return ImVec2(text.x + px(32.0f), px(36.0f));
}

bool button(const char* label, ButtonKind kind, ImVec2 size, bool enabled) {
    const Palette& p = palette();
    Animator& animator = context().animator;
    const std::string_view text = visibleLabel(label);
    const ImVec2 natural = buttonSize(label);
    if (size.x <= 0.0f) size.x = natural.x;
    if (size.y <= 0.0f) size.y = natural.y;

    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(label, size);
    ImGui::EndDisabled();
    const ImGuiID id = ImGui::GetItemID();
    const bool hovered = enabled && ImGui::IsItemHovered();
    const bool held = enabled && ImGui::IsItemActive();

    // Feedback starts on press, not on release.
    const float hover = animator.to(id, hovered ? 1.0f : 0.0f, 0.16f);
    const float press = animator.to(id + 1, held ? 1.0f : 0.0f, 0.10f);

    ImVec4 fill, ink;
    switch (kind) {
        case ButtonKind::Primary:
            fill = mix(p.accent, p.accentHover, hover);
            ink = p.onAccent;
            break;
        case ButtonKind::Secondary:
            fill = mix(p.control, p.controlHover, hover);
            ink = p.text;
            break;
        case ButtonKind::Danger:
            fill = mix(p.control, p.controlHover, hover);
            ink = p.danger;
            break;
        case ButtonKind::Link:
            fill = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.16f * hover);
            ink = mix(ImVec4(0.45f, 0.66f, 1.0f, 1.0f), ImVec4(0.62f, 0.78f, 1.0f, 1.0f), hover);
            break;
        case ButtonKind::Quiet:
        default:
            fill = ImVec4(p.controlHover.x, p.controlHover.y, p.controlHover.z, 0.75f * hover);
            ink = mix(p.textSecondary, p.text, hover);
            break;
    }
    const float alpha = enabled ? 1.0f : 0.4f;

    // A pressed button shrinks by about 3 %.
    const ImVec2 inset(size.x * 0.015f * press, size.y * 0.015f * press);
    const ImVec2 a(min.x + inset.x, min.y + inset.y);
    const ImVec2 b(min.x + size.x - inset.x, min.y + size.y - inset.y);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(a, b, toU32(fill, alpha), px(8.0f));

    const ImVec2 textSize = measure(TextStyle::Strong, text);
    drawText(draw, ImVec2(min.x + (size.x - textSize.x) * 0.5f, min.y + (size.y - textSize.y) * 0.5f),
             TextStyle::Strong, ink, text, alpha);
    return clicked;
}

bool edgeButton(const char* label, ButtonKind kind) {
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - px(16.0f));
    return button(label, kind);
}

bool moreButton(const char* id) {
    const Palette& p = palette();
    const ImVec2 size(px(36.0f), px(36.0f));
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, size);
    const float hover = context().animator.to(ImGui::GetItemID(), ImGui::IsItemHovered() ? 1.0f : 0.0f, 0.16f);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, ImVec2(min.x + size.x, min.y + size.y), toU32(p.controlHover, 0.75f * hover), px(8.0f));
    const ImVec4 ink = mix(p.textTertiary, p.text, hover);
    for (int i = -1; i <= 1; ++i) {
        draw->AddCircleFilled(ImVec2(min.x + size.x * 0.5f + static_cast<float>(i) * px(6.0f), min.y + size.y * 0.5f),
                              px(1.7f), toU32(ink));
    }
    return clicked;
}

// ---- segmented control -----------------------------------------------------

int segmented(const char* id, std::span<const char* const> labels, int selected, float width) {
    const Palette& p = palette();
    Animator& animator = context().animator;
    const float height = px(36.0f);
    const float pad = px(3.0f);

    float widest = 0.0f;
    for (const char* text : labels) widest = std::max(widest, measure(TextStyle::Strong, text).x);
    const float count = static_cast<float>(labels.size());
    float segment = widest + px(28.0f);
    if (width > 0.0f) segment = (width - 2.0f * pad) / count;
    const float total = segment * count + 2.0f * pad;

    ImGui::PushID(id);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, ImVec2(min.x + total, min.y + height), toU32(p.control), px(9.0f));

    // The thumb slides to the chosen segment instead of jumping.
    const float thumbX = animator.to(ImGui::GetID("thumb"), static_cast<float>(selected), 0.24f);
    if (selected >= 0) {
        const float x = min.x + pad + thumbX * segment;
        draw->AddRectFilled(ImVec2(x, min.y + pad), ImVec2(x + segment, min.y + height - pad), toU32(p.accent), px(7.0f));
    }

    int result = selected;
    for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(ImVec2(min.x + pad + static_cast<float>(i) * segment, min.y));
        if (ImGui::InvisibleButton("segment", ImVec2(segment, height))) result = i;
        const bool hovered = ImGui::IsItemHovered();
        const float active = clamp01(1.0f - std::abs(thumbX - static_cast<float>(i)));
        const ImVec4 ink = mix(hovered ? p.text : p.textSecondary, p.onAccent, active);
        const ImVec2 textSize = measure(TextStyle::Strong, labels[i]);
        drawText(draw,
                 ImVec2(min.x + pad + static_cast<float>(i) * segment + (segment - textSize.x) * 0.5f,
                        min.y + (height - textSize.y) * 0.5f),
                 TextStyle::Strong, ink, labels[i]);
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(min);
    ImGui::Dummy(ImVec2(total, height));
    ImGui::PopID();
    return result;
}

// ---- toggle ----------------------------------------------------------------

bool toggleRow(const char* label, bool* value, bool enabled) {
    const Palette& p = palette();
    const float rowHeight = px(36.0f);
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 min = ImGui::GetCursorScreenPos();

    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(width, rowHeight));
    ImGui::EndDisabled();
    if (clicked) *value = !*value;

    const float alpha = enabled ? 1.0f : 0.4f;
    const float on = context().animator.to(ImGui::GetItemID(), *value ? 1.0f : 0.0f, 0.2f);
    ImDrawList* draw = ImGui::GetWindowDrawList();

    const std::string_view text = visibleLabel(label);
    const ImVec2 textSize = measure(TextStyle::Body, text);
    drawText(draw, ImVec2(min.x, min.y + (rowHeight - textSize.y) * 0.5f), TextStyle::Body, p.text, text, alpha);

    const ImVec2 track(px(42.0f), px(24.0f));
    const ImVec2 trackMin(min.x + width - track.x, min.y + (rowHeight - track.y) * 0.5f);
    draw->AddRectFilled(trackMin, ImVec2(trackMin.x + track.x, trackMin.y + track.y),
                        toU32(mix(p.controlHover, p.accent, on), alpha), track.y * 0.5f);
    const float knob = track.y * 0.5f - px(3.0f);
    const float knobX = trackMin.x + track.y * 0.5f + on * (track.x - track.y);
    draw->AddCircleFilled(ImVec2(knobX, trackMin.y + track.y * 0.5f), knob, toU32(p.onAccent, alpha), 24);
    return clicked;
}

// ---- text field ------------------------------------------------------------

bool textField(const char* label, std::string& value, const char* placeholder, FieldState state,
               std::string_view message, ImGuiInputTextFlags flags, float width, TextStyle style) {
    const Palette& p = palette();
    ImGui::PushID(label);
    const std::string_view caption = visibleLabel(label);
    if (!caption.empty()) {
        ui::label(TextStyle::Caption, caption, p.textSecondary);
        space(6.0f);
    }

    if (width <= 0.0f) width = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(width);
    const ImVec4 border = state == FieldState::Invalid ? p.danger
                          : state == FieldState::Valid ? ImVec4(p.ready.x, p.ready.y, p.ready.z, 0.7f)
                                                       : p.hairline;
    ImGui::PushStyleColor(ImGuiCol_Border, border);
    pushText(style);
    const bool changed = ImGui::InputTextWithHint("##field", placeholder, &value, flags);
    popText();
    ImGui::PopStyleColor();

    if (!message.empty()) {
        space(6.0f);
        paragraph(TextStyle::Caption, message, state == FieldState::Invalid ? p.danger : p.textSecondary);
    }
    ImGui::PopID();
    return changed;
}

// ---- slider ----------------------------------------------------------------

bool sliderRow(const char* label, float* value, float min, float max, std::string_view valueText) {
    const Palette& p = palette();
    ImGui::PushID(label);
    const float width = ImGui::GetContentRegionAvail().x;
    ImDrawList* draw = ImGui::GetWindowDrawList();

    const ImVec2 start = ImGui::GetCursorScreenPos();
    ui::label(TextStyle::Body, visibleLabel(label));
    const ImVec2 valueSize = measure(TextStyle::Body, valueText);
    drawText(draw, ImVec2(start.x + width - valueSize.x, start.y), TextStyle::Body, p.textSecondary, valueText);
    space(4.0f);

    // ImGui's slider does the work (mouse, keyboard, gamepad); we only replace its looks.
    const float grab = px(18.0f);
    ImGui::SetNextItemWidth(width);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, px(3.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, grab);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    for (ImGuiCol color : {ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive, ImGuiCol_SliderGrab,
                           ImGuiCol_SliderGrabActive}) {
        ImGui::PushStyleColor(color, ImVec4(0, 0, 0, 0));
    }
    const bool changed = ImGui::SliderFloat("##slider", value, min, max, "", ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(3);

    const float active = context().animator.to(ImGui::GetItemID(),
                                               ImGui::IsItemActive() || ImGui::IsItemHovered() ? 1.0f : 0.0f, 0.16f);
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    const float y = (a.y + b.y) * 0.5f;
    const float t = max > min ? clamp01((*value - min) / (max - min)) : 0.0f;
    const float knobX = a.x + 2.0f + grab * 0.5f + t * (b.x - a.x - 4.0f - grab);
    const float half = px(2.0f);
    draw->AddRectFilled(ImVec2(a.x, y - half), ImVec2(b.x, y + half), toU32(p.control), half);
    draw->AddRectFilled(ImVec2(a.x, y - half), ImVec2(knobX, y + half), toU32(p.accent), half);
    draw->AddCircleFilled(ImVec2(knobX, y), px(8.0f) + px(1.0f) * active, toU32(p.onAccent), 28);
    ImGui::PopID();
    return changed;
}

// ---- small pieces ------------------------------------------------------------

void statusDot(ImDrawList* draw, ImVec2 center, const ImVec4& color, bool pulse) {
    const float radius = px(5.0f);
    if (pulse && !context().animator.reducedMotion()) {
        // A soft ring that grows and fades about once per 1.4 s while waiting.
        const float phase = std::fmod(static_cast<float>(ImGui::GetTime()) / 1.4f, 1.0f);
        draw->AddCircleFilled(center, radius + px(9.0f) * phase, toU32(color, 0.35f * (1.0f - phase)), 32);
    }
    draw->AddCircleFilled(center, radius, toU32(color), 24);
}

void spinner(float radius) {
    const Palette& p = palette();
    const float r = px(radius);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 center(min.x + r, min.y + r);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddCircle(center, r - px(1.5f), toU32(p.control), 48, px(3.0f));
    // With reduced motion the arc stands still; the text next to it says what is going on.
    const float start = context().animator.reducedMotion() ? 0.0f : static_cast<float>(ImGui::GetTime()) * 5.0f;
    draw->PathArcTo(center, r - px(1.5f), start, start + kTwoPi * 0.28f, 24);
    draw->PathStroke(toU32(p.accentHover), 0, px(3.0f));
    ImGui::Dummy(ImVec2(r * 2.0f, r * 2.0f));
}

void drawGear(ImDrawList* draw, ImVec2 center, float radius, ImU32 color) {
    const float stroke = std::max(1.5f, radius * 0.26f);
    draw->AddCircle(center, radius * 0.56f, color, 0, stroke);
    for (int tooth = 0; tooth < 8; ++tooth) {
        const float angle = static_cast<float>(tooth) * 0.78539816f;
        const ImVec2 direction(std::cos(angle), std::sin(angle));
        draw->AddLine(ImVec2(center.x + direction.x * radius * 0.66f, center.y + direction.y * radius * 0.66f),
                      ImVec2(center.x + direction.x * radius, center.y + direction.y * radius), color, stroke * 1.25f);
    }
}

void drawLogo(ImDrawList* draw, ImVec2 min, float size) {
    // In units of the tile's edge; see tools/make_icon.py.
    constexpr float kTileRadius = 0.225f;
    constexpr float kHeight = 0.52f;
    constexpr float kWidth = 0.44f;
    constexpr float kCorner = 0.035f;
    constexpr float kLayers[3][2] = {{0.18f, 0.28f}, {0.28f, 0.55f}, {0.38f, 1.0f}}; // left edge, opacity

    // The icon file has a slight gradient on the tile. At the size the mark
    // has inside the client it cannot be seen, so one colour does.
    draw->AddRectFilled(min, ImVec2(min.x + size, min.y + size), IM_COL32(0x1b, 0x1f, 0x26, 255), kTileRadius * size);

    const float top = 0.5f - kHeight * 0.5f;
    for (const auto& layer : kLayers) {
        // Corners of the triangle, clockwise on screen, tip to the right.
        const ImVec2 corners[3] = {ImVec2(layer[0], top), ImVec2(layer[0] + kWidth, 0.5f),
                                   ImVec2(layer[0], top + kHeight)};
        // Outward normals of the three edges.
        ImVec2 normals[3];
        for (int i = 0; i < 3; ++i) {
            const ImVec2 a = corners[i];
            const ImVec2 b = corners[(i + 1) % 3];
            const float dx = b.x - a.x;
            const float dy = b.y - a.y;
            const float length = std::sqrt(dx * dx + dy * dy);
            normals[i] = ImVec2(dy / length, -dx / length);
        }
        // Rounded corners: an arc around each corner of the triangle moved
        // inwards by the radius, from the normal of the edge before it to
        // the normal of the edge after it.
        draw->PathClear();
        for (int i = 0; i < 3; ++i) {
            const ImVec2 before = normals[(i + 2) % 3];
            const ImVec2 after = normals[i];
            // Intersection of the two edges, each moved inwards by kCorner.
            const ImVec2 p0 = corners[(i + 2) % 3];
            const ImVec2 p1 = corners[i];
            const float c0 = before.x * (p0.x - before.x * kCorner) + before.y * (p0.y - before.y * kCorner);
            const float c1 = after.x * (p1.x - after.x * kCorner) + after.y * (p1.y - after.y * kCorner);
            const float det = before.x * after.y - after.x * before.y;
            const ImVec2 centre((c0 * after.y - c1 * before.y) / det, (before.x * c1 - after.x * c0) / det);
            float from = std::atan2(before.y, before.x);
            float to = std::atan2(after.y, after.x);
            if (to < from) to += 6.2831853f;
            draw->PathArcTo(ImVec2(min.x + centre.x * size, min.y + centre.y * size), kCorner * size, from, to, 8);
        }
        draw->PathFillConvex(IM_COL32(0x2a, 0x6f, 0xe8, static_cast<int>(layer[1] * 255.0f)));
    }
}

void hairline() {
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(min, ImVec2(min.x + width, min.y), toU32(palette().hairline), 1.0f);
    ImGui::Dummy(ImVec2(width, 1.0f));
}

} // namespace veloplay::ui
