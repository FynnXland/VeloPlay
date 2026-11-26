# The Linux shell around the client: window, loop, keys. The program itself is
# defined further down, once the other parts have added their sources.
target_sources(veloplay_linux PRIVATE
    ${PROJECT_SOURCE_DIR}/src/linux/KeyMap.cpp
    ${PROJECT_SOURCE_DIR}/src/input/SdlControllerBackend.cpp)
