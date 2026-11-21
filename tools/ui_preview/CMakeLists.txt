# Renders every screen of the client into image files, without a console, a
# controller or Windows. Used to review UI changes and to make screenshots.
find_package(glfw3 REQUIRED)
find_package(OpenGL REQUIRED)

add_executable(ui_preview
    main.cpp
    ${imgui_src_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_src_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_link_libraries(ui_preview PRIVATE veloplay_ui glfw OpenGL::GL)
