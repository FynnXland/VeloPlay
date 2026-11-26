# The system services of the Linux client: HTTPS, the keyring, installing,
# Discord, sound and the desktop. Included from the "Linux" block of the root
# CMakeLists.txt; adds to the target veloplay_linux.

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)

target_sources(veloplay_linux PRIVATE
    ${PROJECT_SOURCE_DIR}/src/linux/CurlHttpClient.cpp
    ${PROJECT_SOURCE_DIR}/src/linux/SecretServiceProtector.cpp
    ${PROJECT_SOURCE_DIR}/src/linux/DesktopLinux.cpp
    ${PROJECT_SOURCE_DIR}/src/linux/SdlAudio.cpp
    ${PROJECT_SOURCE_DIR}/src/linux/InstallerLinux.cpp
    ${PROJECT_SOURCE_DIR}/src/linux/DiscordPipeLinux.cpp
)
# discord::createPipe() comes from DiscordPipeLinux.cpp; the core's stand-in for
# other systems must not be there as well.
target_compile_definitions(veloplay_core PRIVATE VELOPLAY_DISCORD_PIPE_ELSEWHERE)

# The icon of the package, next to the program: the 256 x 256 picture of the
# Windows icon (assets/veloplay.ico), so both platforms show the same one and
# no image tool is needed to build. The installer hands it to the desktop.
set(VELOPLAY_LINUX_ICON "${PROJECT_SOURCE_DIR}/assets/linux/veloplay.png")
if(VELOPLAY_BUILD_LINUX_APP)
    install(FILES "${VELOPLAY_LINUX_ICON}" DESTINATION .)
endif()

# libsecret's headers only, for its types and declarations: the library itself is
# opened with dlopen, so the program also starts where no keyring is installed.
find_package(PkgConfig REQUIRED)
pkg_check_modules(VELOPLAY_LIBSECRET REQUIRED libsecret-1)
target_include_directories(veloplay_linux SYSTEM PRIVATE ${VELOPLAY_LIBSECRET_INCLUDE_DIRS})

# OpenSSL::SSL only for the tests' own HTTPS server (they link this library);
# the program's TLS lives inside curl.
target_link_libraries(veloplay_linux PUBLIC CURL::libcurl OpenSSL::SSL)
