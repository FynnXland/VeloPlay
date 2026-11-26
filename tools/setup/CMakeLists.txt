# VeloPlay-Setup.exe: the file to double-click (see main.cpp). This builds the
# program part; the release attaches the package to it.
#
# It runs before anything is unpacked, so it links nothing that would have to
# lie next to it: the C++ runtime goes in statically, and miniz is compiled in
# from source (the one vcpkg provides is a DLL here).
include(FetchContent)
FetchContent_Declare(veloplay_setup_miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG 3.1.2
    GIT_SHALLOW TRUE
    # Only the sources are wanted. Its own build would make another target named miniz.
    SOURCE_SUBDIR no-build-of-its-own)
FetchContent_MakeAvailable(veloplay_setup_miniz)
set(setup_miniz "${veloplay_setup_miniz_SOURCE_DIR}")
# miniz' build writes this header; without its build it is written here.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/miniz/miniz_export.h" "#pragma once\n#define MINIZ_EXPORT\n")

configure_file(setup.rc.in "${CMAKE_CURRENT_BINARY_DIR}/setup.rc" @ONLY)

add_executable(veloplay_setup WIN32
    main.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/setup.rc"
    "${PROJECT_SOURCE_DIR}/src/setup/Payload.cpp"
    "${PROJECT_SOURCE_DIR}/src/update/Zip.cpp"
    "${setup_miniz}/miniz.c"
    "${setup_miniz}/miniz_zip.c"
    "${setup_miniz}/miniz_tinfl.c"
    "${setup_miniz}/miniz_tdef.c")
target_include_directories(veloplay_setup PRIVATE
    "${PROJECT_SOURCE_DIR}/src" "${setup_miniz}" "${CMAKE_CURRENT_BINARY_DIR}/miniz")
target_link_libraries(veloplay_setup PRIVATE veloplay_options shell32 ole32 advapi32 user32 gdi32 uuid)
set_target_properties(veloplay_setup PROPERTIES
    OUTPUT_NAME "VeloPlay-Setup-Program"
    # Not next to VeloPlay.exe: the package is everything in that folder.
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/setup"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT MSVC)
    target_link_options(veloplay_setup PRIVATE -municode -static -s)
endif()
