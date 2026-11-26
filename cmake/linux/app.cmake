# The Linux client: the window and loop of src/linux on top of every part of veloplay_linux.
# Until every part it uses has arrived (they are written side by side), the program is left out.
set(veloplay_linux_parts_there TRUE)
foreach(part GlVideoRenderer GlCapture SdlAudio LinuxServices DesktopLinux FfmpegRecorder V4l2Capture)
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/src/linux/${part}.hpp")
        set(veloplay_linux_parts_there FALSE)
    endif()
endforeach()
if(VELOPLAY_BUILD_LINUX_APP AND NOT veloplay_linux_parts_there)
    message(STATUS "The Linux client is left out: not all of its parts are there yet.")
endif()
if(VELOPLAY_BUILD_LINUX_APP AND veloplay_linux_parts_there)
    if(NOT VELOPLAY_BUILD_UI_PREVIEW AND NOT TARGET veloplay_ui)
        message(FATAL_ERROR "The Linux client needs the UI library.")
    endif()
    add_executable(veloplay_linux_app
        ${PROJECT_SOURCE_DIR}/src/linux/main.cpp
        ${PROJECT_SOURCE_DIR}/src/linux/LinuxApplication.cpp
        ${imgui_src_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_src_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
    target_link_libraries(veloplay_linux_app PRIVATE veloplay_ui veloplay_linux SDL3::SDL3)
    set_target_properties(veloplay_linux_app PROPERTIES
        OUTPUT_NAME "VeloPlay"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/app")
    install(TARGETS veloplay_linux_app RUNTIME DESTINATION .)
    # The icon beside the program, as the package has it (cmake/linux/services.cmake installs it).
    add_custom_command(TARGET veloplay_linux_app POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${PROJECT_SOURCE_DIR}/assets/linux/veloplay.png $<TARGET_FILE_DIR:veloplay_linux_app>/veloplay.png)
    install(FILES ${PROJECT_SOURCE_DIR}/README.md ${PROJECT_SOURCE_DIR}/README.de.md ${PROJECT_SOURCE_DIR}/CHANGELOG.md
        ${PROJECT_SOURCE_DIR}/LICENSE ${PROJECT_SOURCE_DIR}/THIRD_PARTY.md DESTINATION .)
endif()
