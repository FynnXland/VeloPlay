# miniz reads the update archive. vcpkg provides it on Windows; distributions
# rarely package it, so other builds fetch the same version.
find_package(miniz CONFIG QUIET)
if(NOT TARGET miniz::miniz)
    include(FetchContent)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(INSTALL_PROJECT OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(miniz_src
        GIT_REPOSITORY https://github.com/richgel999/miniz.git
        GIT_TAG 3.1.2
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(miniz_src)
    add_library(miniz::miniz ALIAS miniz)
endif()
