# FFmpeg (video decoding) and Opus (audio decoding). vcpkg provides both on
# Windows; elsewhere the system packages are found through pkg-config.
#
# Defines the targets veloplay_ffmpeg and veloplay_opus.

add_library(veloplay_ffmpeg INTERFACE)
add_library(veloplay_opus INTERFACE)

if(DEFINED VCPKG_TOOLCHAIN OR DEFINED VCPKG_INSTALLED_DIR)
    find_package(FFMPEG REQUIRED)
    target_include_directories(veloplay_ffmpeg SYSTEM INTERFACE ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(veloplay_ffmpeg INTERFACE ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(veloplay_ffmpeg INTERFACE ${FFMPEG_LIBRARIES})

    find_package(Opus CONFIG REQUIRED)
    target_link_libraries(veloplay_opus INTERFACE Opus::opus)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(VELOPLAY_AV REQUIRED IMPORTED_TARGET libavcodec libavutil)
    target_link_libraries(veloplay_ffmpeg INTERFACE PkgConfig::VELOPLAY_AV)

    pkg_check_modules(VELOPLAY_OPUS REQUIRED IMPORTED_TARGET opus)
    target_link_libraries(veloplay_opus INTERFACE PkgConfig::VELOPLAY_OPUS)
endif()
