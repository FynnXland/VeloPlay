#pragma once

#include <span>
#include <string>
#include <vector>
#include <string_view>
#include <unordered_map>

#include <imgui.h>

#include "ui/Strings.hpp"
#include "ui/Theme.hpp"

// The handful of controls the client draws itself, so that buttons, switches
// and segmented controls share one look and one way of moving.

namespace veloplay::ui {

/// Smooths values over time with critically damped springs: no overshoot, and
/// a change of target mid-flight continues from the current position and speed.
class Animator {
public:
    /// Returns the animated value for `id`, heading for `target`. `response`
    /// is roughly the time to arrive, in seconds. The first call for an id
    /// starts at `initial` (or at the target if none is given).
    float to(ImGuiID id, float target, float response = 0.22f);
    float to(ImGuiID id, float target, float response, float initial);

    void setReducedMotion(bool reduced) { reducedMotion_ = reduced; }
    [[nodiscard]] bool reducedMotion() const { return reducedMotion_; }

    void forget(ImGuiID id) { springs_.erase(id); }

private:
    struct Spring {
        float value = 0.0f;
        float velocity = 0.0f;
    };
    std::unordered_map<ImGuiID, Spring> springs_;
    bool reducedMotion_ = false;
};

/// Per-frame state shared by all widgets.
/// A check of the drawing itself, for the tool that renders every screen:
/// whatever is drawn partly outside the area its window may draw in gets
/// noted. That is how the rounded corners of a button went missing on one
/// side: the button stood sixteen pixels left of its column, the column cut
/// it off, and it showed only while the pointer was on it.
struct ClipAudit {
    bool enabled = false;
    std::vector<std::string> findings;
};

struct Context {
    Fonts fonts;
    float scale = 1.0f;
    const Strings* text = nullptr;
    Animator animator;
    ClipAudit audit;
};

Context& context();

/// Converts a length given for 100 % scaling to pixels.
float px(float value);

/// A filled rectangle. Every surface of the interface is drawn through this, so the check above sees it,
/// also while it is fully transparent (a button nobody points at).
void fill(ImDrawList* draw, ImVec2 a, ImVec2 b, ImU32 color, float rounding = 0.0f, ImDrawFlags flags = 0);
/// Notes the rectangle if the current window cuts part of it off. Does nothing unless the check is on.
void auditClip(ImDrawList* draw, ImVec2 a, ImVec2 b, const char* what);

void space(float height);

// ---- text ----------------------------------------------------------------

void pushText(TextStyle style);
void popText();

void label(TextStyle style, std::string_view text, const ImVec4& color);
void label(TextStyle style, std::string_view text);

/// Wraps at the right edge of the current content region.
void paragraph(TextStyle style, std::string_view text, const ImVec4& color);

ImVec2 measure(TextStyle style, std::string_view text);

void drawText(ImDrawList* draw, ImVec2 position, TextStyle style, const ImVec4& color, std::string_view text,
              float alpha = 1.0f);

// ---- controls ------------------------------------------------------------

enum class ButtonKind {
    Primary,   // the one action a screen is about
    Secondary, // other actions
    Quiet,     // low-key actions: back, cancel, menus
    Link,      // opens help or documentation
    Danger,
};

/// `label` may carry an ImGui id suffix ("Remove##console-3").
bool button(const char* label, ButtonKind kind, ImVec2 size = ImVec2(0, 0), bool enabled = true);

ImVec2 buttonSize(const char* label);

/// A Quiet or Link button whose text lines up with the left edge of the
/// content. The hover background reaches into the margin instead.
bool edgeButton(const char* label, ButtonKind kind = ButtonKind::Quiet);

/// A square quiet button showing three dots, for row menus.
bool moreButton(const char* id);

/// Returns the index chosen this frame, or `selected` if nothing changed.
/// `disabled` has one bit per segment: those are greyed out and cannot be chosen.
int segmented(const char* id, std::span<const char* const> labels, int selected, float width = 0.0f,
              unsigned disabled = 0);
/// A small rounded label in a colour, on the line where the cursor is: "Beta", "NVIDIA".
void pill(std::string_view text, const ImVec4& color);

/// Label on the left, switch on the right, the whole row clickable.
bool toggleRow(const char* label, bool* value, bool enabled = true);

enum class FieldState { Neutral, Valid, Invalid };

/// Single-line text field with a caption above and an optional message below.
/// A label that starts with "##" is used as id only and not shown.
bool textField(const char* label, std::string& value, const char* placeholder, FieldState state,
               std::string_view message, ImGuiInputTextFlags flags = 0, float width = 0.0f,
               TextStyle style = TextStyle::Body);

/// Slider with its caption and current value on one line above the track.
bool sliderRow(const char* label, float* value, float min, float max, std::string_view valueText);

void statusDot(ImDrawList* draw, ImVec2 center, const ImVec4& color, bool pulse);

/// A gear, the usual sign for settings.
void drawGear(ImDrawList* draw, ImVec2 center, float radius, ImU32 color);

/// The VeloPlay mark in a square of `size` pixels: a play triangle with two
/// fainter copies behind it, on a dark tile. Same numbers as tools/make_icon.py.
void drawLogo(ImDrawList* draw, ImVec2 min, float size);

void spinner(float radius);

void hairline();

} // namespace veloplay::ui
