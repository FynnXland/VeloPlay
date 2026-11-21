# Dear ImGui for builds without vcpkg (the UI preview on Linux and macOS).
# The Windows client takes the same version from vcpkg.
include(FetchContent)

FetchContent_Declare(imgui_src
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.9
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui_src)

add_library(veloplay_imgui STATIC
    ${imgui_src_SOURCE_DIR}/imgui.cpp
    ${imgui_src_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_src_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_src_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_src_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp)
target_include_directories(veloplay_imgui SYSTEM PUBLIC
    ${imgui_src_SOURCE_DIR}
    ${imgui_src_SOURCE_DIR}/misc/cpp
    ${imgui_src_SOURCE_DIR}/backends)
