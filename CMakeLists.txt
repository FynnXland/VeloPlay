cmake_minimum_required(VERSION 3.25)

project(VeloPlay
    VERSION 1.1.0
    DESCRIPTION "Remote Play client for PS5 with frame generation and upscaling"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(VELOPLAY_BUILD_APP "Build the Windows client (needs Direct3D 11, Dear ImGui, SDL3)" ${WIN32})
option(VELOPLAY_BUILD_CLI "Build veloplay-cli, a diagnostics tool for discovery, wake-up and pairing" ON)
option(VELOPLAY_BUILD_TESTS "Build the unit tests" ON)
option(VELOPLAY_BUILD_UI_PREVIEW "Build ui_preview, which renders the UI offscreen (needs GLFW and OpenGL)" OFF)

if(VELOPLAY_BUILD_APP AND NOT WIN32)
    message(FATAL_ERROR "The client application targets Windows. Configure with -DVELOPLAY_BUILD_APP=OFF to build the portable parts.")
endif()

# ---- Compiler settings shared by all targets -------------------------------
add_library(veloplay_options INTERFACE)
if(MSVC)
    target_compile_options(veloplay_options INTERFACE /W4 /permissive- /utf-8 /MP)
    target_compile_definitions(veloplay_options INTERFACE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(veloplay_options INTERFACE -Wall -Wextra -Wpedantic)
    if(WIN32)
        target_compile_definitions(veloplay_options INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    endif()
endif()

find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(Threads REQUIRED)
include(cmake/FetchMiniz.cmake)
include(cmake/FindMedia.cmake)

# Session and transport of the Remote Play protocol (libchiaki from chiaki-ng).
add_subdirectory(third_party/chiaki)

# ---- Core: protocol, input model, configuration (no platform UI) -----------
add_library(veloplay_core STATIC
    src/util/Bytes.cpp
    src/util/StallWatch.cpp
    src/util/LogFiles.cpp
    src/net/Socket.cpp
    src/protocol/HttpMessage.cpp
    src/protocol/Discovery.cpp
    src/protocol/DiscoveryService.cpp
    src/protocol/Crypto.cpp
    src/protocol/Registration.cpp
    src/protocol/RegistrationClient.cpp
    src/protocol/LatencyStats.cpp
    src/protocol/ConnectionProbe.cpp
    src/protocol/PsnAccount.cpp
    src/update/Version.cpp
    src/update/Release.cpp
    src/update/Zip.cpp
    src/update/Updater.cpp
    src/input/ControllerState.cpp
    src/input/InputMerger.cpp
    src/input/KeyboardMouse.cpp
    src/input/VirtualController.cpp
    src/input/ControllerManager.cpp
    src/input/RemoteControl.cpp
    src/session/StreamMeter.cpp
    src/media/ColorMatrix.cpp
    src/media/VideoDecoder.cpp
    src/media/Audio.cpp
    src/media/MediaPipeline.cpp
    src/media/FramePacer.cpp
    src/input/DualSenseEffects.cpp
    src/session/ChiakiSession.cpp
    src/app/AutoStream.cpp
    src/app/StreamAlerts.cpp
    src/app/StreamProfile.cpp
    src/app/Config.cpp
    src/app/AppState.cpp
    src/app/ConsoleList.cpp
    src/app/AppController.cpp
    src/app/HelpLinks.cpp
    src/app/Installer.cpp
    # Texts live with the UI but have no toolkit dependency, so the tests can check them.
    src/ui/Strings.cpp
)
target_include_directories(veloplay_core PUBLIC src)
target_compile_definitions(veloplay_core PUBLIC VELOPLAY_VERSION="${PROJECT_VERSION}")
target_link_libraries(veloplay_core
    PUBLIC veloplay_options OpenSSL::Crypto nlohmann_json::nlohmann_json spdlog::spdlog Threads::Threads
    PRIVATE miniz::miniz veloplay_chiaki veloplay_ffmpeg veloplay_opus)
if(WIN32)
    target_sources(veloplay_core PRIVATE src/app/DpapiProtector.cpp src/update/WinHttpClient.cpp
        src/media/D3D11VaDevice.cpp src/util/ThreadStackWin.cpp)
    target_link_libraries(veloplay_core PUBLIC ws2_32 iphlpapi crypt32 winhttp dbghelp)
endif()

# ---- UI: Dear ImGui screens, independent of window system and renderer -----
if(VELOPLAY_BUILD_APP OR VELOPLAY_BUILD_UI_PREVIEW)
    if(VELOPLAY_BUILD_APP)
        find_package(imgui CONFIG REQUIRED)
        set(VELOPLAY_IMGUI_TARGET imgui::imgui)
    else()
        include(cmake/FetchImGui.cmake)
        set(VELOPLAY_IMGUI_TARGET veloplay_imgui)
    endif()

    add_library(veloplay_ui STATIC
        src/ui/Theme.cpp
        src/ui/Widgets.cpp
        src/ui/UiManager.cpp
    )
    target_link_libraries(veloplay_ui PUBLIC veloplay_core ${VELOPLAY_IMGUI_TARGET})
endif()

# ---- Windows: picture and sound output --------------------------------------
# A library of its own so the tests can draw with the software renderer.
if(WIN32)
    add_library(veloplay_platform STATIC
        src/render/D3D11Device.cpp
        src/render/VideoRenderer.cpp
        src/render/FrameGenerator.cpp
        src/render/NvofMotion.cpp
        src/render/RtxVideoScaler.cpp
        src/audio/WasapiInput.cpp
        src/audio/WasapiOutput.cpp
    )
    target_link_libraries(veloplay_platform PUBLIC veloplay_core d3d11 dxgi d3dcompiler ole32 avrt)
    # The interface of NVIDIA's optical flow engine: headers only, the library is part of the driver.
    target_include_directories(veloplay_platform PRIVATE third_party/nvof)
endif()

# ---- Windows client ---------------------------------------------------------
if(VELOPLAY_BUILD_APP)
    find_package(SDL3 CONFIG REQUIRED)
    # The embedded browser for signing in at Sony (account ID lookup).
    find_package(unofficial-webview2 CONFIG REQUIRED)

    add_executable(veloplay WIN32
        src/main.cpp
        src/app/Application.cpp
        src/app/HangProbe.cpp
        src/app/InstallerWin.cpp
        src/app/LoginWindow.cpp
        src/input/SdlControllerBackend.cpp
        src/app/veloplay.manifest
        src/app/veloplay.rc
    )
    target_link_libraries(veloplay PRIVATE veloplay_ui veloplay_platform SDL3::SDL3 unofficial::webview2::webview2
        dwmapi ole32 winmm)
    set_target_properties(veloplay PROPERTIES
        OUTPUT_NAME "VeloPlay"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/app")

    install(TARGETS veloplay RUNTIME DESTINATION .)
    # vcpkg copies the DLLs of all dependencies next to the executable.
    install(DIRECTORY "$<TARGET_FILE_DIR:veloplay>/" DESTINATION . FILES_MATCHING PATTERN "*.dll")
    install(FILES README.md README.de.md CHANGELOG.md LICENSE THIRD_PARTY.md DESTINATION .)
endif()

if(VELOPLAY_BUILD_CLI)
    add_executable(veloplay-cli tools/cli/main.cpp)
    target_link_libraries(veloplay-cli PRIVATE veloplay_core)
    if(VELOPLAY_BUILD_APP)
        set_target_properties(veloplay-cli PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/app")
        install(TARGETS veloplay-cli RUNTIME DESTINATION .)
    endif()
endif()

if(VELOPLAY_BUILD_UI_PREVIEW)
    add_subdirectory(tools/ui_preview)
endif()

if(VELOPLAY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
