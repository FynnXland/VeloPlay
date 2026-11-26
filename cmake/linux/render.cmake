# The picture on Linux: OpenGL 3.3 core, the counterpart of src/render (Direct3D 11).
# The functions are loaded at run time through the window's loader (GlFunctions), so
# nothing is linked here: OpenGL comes through the loader the shell passes in.
target_sources(veloplay_linux PRIVATE
    ${PROJECT_SOURCE_DIR}/src/linux/GlFunctions.cpp
    ${PROJECT_SOURCE_DIR}/src/linux/GlVideoRenderer.cpp
    ${PROJECT_SOURCE_DIR}/src/linux/GlFrameGenerator.cpp
    ${PROJECT_SOURCE_DIR}/src/linux/GlCapture.cpp
)
