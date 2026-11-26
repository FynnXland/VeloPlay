# Linux: recording a video (FFmpeg) and reading a capture card (V4L2).
#
# The root CMakeLists links avcodec and avutil (veloplay_ffmpeg) privately
# into the core. Writing an MP4 and turning pixels from one layout into
# another needs two more libraries, avformat and swscale; this file adds them
# as veloplay_ffmpeg_media.
#
# With vcpkg, FindFFMPEG puts every library the port was built with into
# FFMPEG_LIBRARIES already, so veloplay_ffmpeg carries them; the port must be
# built with the features "avformat" and "swscale" (and an H.264 encoder:
# "openh264", or "x264", which makes the whole FFmpeg GPL).

add_library(veloplay_ffmpeg_media INTERFACE)

if(DEFINED VCPKG_TOOLCHAIN OR DEFINED VCPKG_INSTALLED_DIR)
    if(NOT FFMPEG_libavformat_FOUND OR NOT FFMPEG_libswscale_FOUND)
        message(FATAL_ERROR "The Linux recorder and capture card need FFmpeg from vcpkg with the features "
                            "\"avformat\" and \"swscale\".")
    endif()
    target_link_libraries(veloplay_ffmpeg_media INTERFACE veloplay_ffmpeg)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(VELOPLAY_LINUX_MEDIA_AV REQUIRED IMPORTED_TARGET libavformat libavcodec libswscale libavutil)
    target_link_libraries(veloplay_ffmpeg_media INTERFACE PkgConfig::VELOPLAY_LINUX_MEDIA_AV)
endif()

target_sources(veloplay_linux PRIVATE
    ${PROJECT_SOURCE_DIR}/src/linux/FfmpegRecorder.cpp
    ${PROJECT_SOURCE_DIR}/src/linux/V4l2Capture.cpp
)
# Public: the tests read recordings back with avformat and make JPEG pictures with avcodec.
target_link_libraries(veloplay_linux PUBLIC veloplay_ffmpeg_media)
