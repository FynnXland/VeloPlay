#include "ui/Theme.hpp"

#include <cstdio>

namespace veloplay::ui {

namespace {

constexpr ImVec4 rgb(unsigned hex, float alpha = 1.0f) {
    return ImVec4(static_cast<float>((hex >> 16) & 0xff) / 255.0f, static_cast<float>((hex >> 8) & 0xff) / 255.0f,
                  static_cast<float>(hex & 0xff) / 255.0f, alpha);
}

constexpr Palette kPalette = {
    .background = rgb(0x0e0f11),
    .surface = rgb(0x17191c),
    .surfaceRaised = rgb(0x1b1d21),
    .control = rgb(0x26292e),
    .controlHover = rgb(0x30343a),
    .hairline = rgb(0xffffff, 0.07f),
    .text = rgb(0xf2f3f5),
    .textSecondary = rgb(0xa3a9b2),
    .textTertiary = rgb(0x737a84),
    .accent = rgb(0x2a6fe8),
    .accentHover = rgb(0x3d7ef0),
    .onAccent = rgb(0xffffff),
    .ready = rgb(0x34c759),
    .standby = rgb(0xff9f0a),
    .offline = rgb(0x6b717a),
    .danger = rgb(0xff5a4f),
    .beta = rgb(0xffd60a),
    .scrim = rgb(0x000000, 0.55f),
};

bool fileExists(const char* path) {
    if (!path || !*path) return false;
    if (std::FILE* file = std::fopen(path, "rb")) {
        std::fclose(file);
        return true;
    }
    return false;
}

} // namespace

const Palette& palette() { return kPalette; }

ImU32 toU32(const ImVec4& color, float alpha) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(color.x, color.y, color.z, color.w * alpha));
}

ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

Fonts loadFonts(const char* regularPath, const char* semiboldPath) {
    ImGuiIO& io = ImGui::GetIO();
    Fonts fonts;
    if (fileExists(regularPath)) fonts.regular = io.Fonts->AddFontFromFileTTF(regularPath, 15.0f);
    if (!fonts.regular) fonts.regular = io.Fonts->AddFontDefault();
    if (fileExists(semiboldPath)) fonts.semibold = io.Fonts->AddFontFromFileTTF(semiboldPath, 15.0f);
    if (!fonts.semibold) fonts.semibold = fonts.regular;
    io.FontDefault = fonts.regular;
    return fonts;
}

TextSpec textSpec(TextStyle style, const Fonts& fonts) {
    switch (style) {
        case TextStyle::Display: return {fonts.semibold, 23.0f};
        case TextStyle::Title: return {fonts.semibold, 20.0f};
        case TextStyle::Strong: return {fonts.semibold, 15.0f};
        case TextStyle::Body: return {fonts.regular, 15.0f};
        case TextStyle::Caption: return {fonts.regular, 13.0f};
    }
    return {fonts.regular, 15.0f};
}

void applyTheme(float scale) {
    const Palette& p = palette();
    ImGuiStyle style; // start from defaults so repeated calls do not compound the scaling

    style.WindowPadding = ImVec2(0, 0);
    style.WindowBorderSize = 0.0f;
    style.WindowRounding = 0.0f;
    style.ChildRounding = 14.0f;
    style.ChildBorderSize = 0.0f;
    style.PopupRounding = 12.0f;
    style.PopupBorderSize = 1.0f;
    style.FramePadding = ImVec2(12, 9);
    style.FrameRounding = 8.0f;
    style.FrameBorderSize = 1.0f;
    style.ItemSpacing = ImVec2(8, 8);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.GrabRounding = 8.0f;
    style.GrabMinSize = 18.0f;
    style.ScrollbarSize = 10.0f;
    style.ScrollbarRounding = 8.0f;
    style.DisabledAlpha = 0.4f;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    style.FontSizeBase = 15.0f;

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = p.text;
    c[ImGuiCol_TextDisabled] = p.textTertiary;
    c[ImGuiCol_WindowBg] = p.background;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = p.surfaceRaised;
    c[ImGuiCol_Border] = p.hairline;
    c[ImGuiCol_FrameBg] = p.control;
    c[ImGuiCol_FrameBgHovered] = p.controlHover;
    c[ImGuiCol_FrameBgActive] = p.controlHover;
    c[ImGuiCol_Button] = p.control;
    c[ImGuiCol_ButtonHovered] = p.controlHover;
    c[ImGuiCol_ButtonActive] = p.controlHover;
    c[ImGuiCol_Header] = p.control;
    c[ImGuiCol_HeaderHovered] = p.controlHover;
    c[ImGuiCol_HeaderActive] = p.controlHover;
    c[ImGuiCol_SliderGrab] = p.accent;
    c[ImGuiCol_SliderGrabActive] = p.accentHover;
    c[ImGuiCol_CheckMark] = p.accent;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = p.control;
    c[ImGuiCol_ScrollbarGrabHovered] = p.controlHover;
    c[ImGuiCol_ScrollbarGrabActive] = p.controlHover;
    c[ImGuiCol_TextSelectedBg] = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.45f);
    c[ImGuiCol_NavCursor] = p.accentHover;
    c[ImGuiCol_ModalWindowDimBg] = p.scrim;
    c[ImGuiCol_Separator] = p.hairline;

    ImGui::GetStyle() = style;
}

} // namespace veloplay::ui
