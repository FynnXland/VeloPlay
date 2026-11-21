#pragma once

#include <imgui.h>

// Visual constants of the client. One dark palette, one accent colour, the
// system typeface. Sizes are given for 100 % scaling; ImGui scales fonts and
// px() scales everything drawn by hand.

namespace veloplay::ui {

struct Palette {
    ImVec4 background;
    ImVec4 surface;       // rows on the dashboard
    ImVec4 surfaceRaised; // sheets
    ImVec4 control;       // secondary buttons, fields, segmented tracks
    ImVec4 controlHover;
    ImVec4 hairline;
    ImVec4 text;
    ImVec4 textSecondary;
    ImVec4 textTertiary;
    ImVec4 accent;
    ImVec4 accentHover;
    ImVec4 onAccent;
    ImVec4 ready;
    ImVec4 standby;
    ImVec4 offline;
    ImVec4 danger;
    ImVec4 scrim;
};

const Palette& palette();

ImU32 toU32(const ImVec4& color, float alpha = 1.0f);
ImVec4 mix(const ImVec4& a, const ImVec4& b, float t);

struct Fonts {
    ImFont* regular = nullptr;
    ImFont* semibold = nullptr;
};

/// Loads the two weights from font files. A missing file falls back to the
/// regular face, a missing regular face to ImGui's built-in font.
Fonts loadFonts(const char* regularPath, const char* semiboldPath);

enum class TextStyle {
    Display, // console names
    Title,   // sheet titles
    Strong,  // buttons, field labels that need weight
    Body,
    Caption,
};

struct TextSpec {
    ImFont* font;
    float size; // unscaled
};

TextSpec textSpec(TextStyle style, const Fonts& fonts);

/// Sets ImGui's style for the given display scale (1.0 = 96 dpi).
void applyTheme(float scale);

} // namespace veloplay::ui
