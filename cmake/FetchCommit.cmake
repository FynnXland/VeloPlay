# Fetches one commit of a git repository into the build tree, without history.
# FetchContent cannot make a shallow clone of a commit that is not a branch or
# tag, and the repositories pinned here are large.
#
#   veloplay_fetch_commit(<name> <repository> <commit> <out-var>)
#
# Set VELOPLAY_<NAME>_SOURCE_DIR to use a local checkout instead (offline builds).
function(veloplay_fetch_commit name repository commit out_var)
    string(TOUPPER "${name}" upper)
    string(REPLACE "-" "_" upper "${upper}")
    set(override "VELOPLAY_${upper}_SOURCE_DIR")
    set(${override} "" CACHE PATH "Local checkout of ${name}; empty downloads commit ${commit}")
    if(${override})
        if(NOT IS_DIRECTORY "${${override}}")
            message(FATAL_ERROR "${override} points to '${${override}}', which is not a directory")
        endif()
        set(${out_var} "${${override}}" PARENT_SCOPE)
        return()
    endif()

    set(dir "${CMAKE_BINARY_DIR}/_deps/${name}-${commit}")
    set(stamp "${dir}/.veloplay-fetched")
    if(NOT EXISTS "${stamp}")
        find_package(Git REQUIRED)
        file(REMOVE_RECURSE "${dir}")
        file(MAKE_DIRECTORY "${dir}")
        message(STATUS "Fetching ${name} at ${commit}")
        execute_process(COMMAND "${GIT_EXECUTABLE}" init --quiet WORKING_DIRECTORY "${dir}" RESULT_VARIABLE result)
        if(result EQUAL 0)
            # A flaky network should not fail the whole configure step.
            foreach(attempt RANGE 1 3)
                execute_process(COMMAND "${GIT_EXECUTABLE}" fetch --quiet --depth 1 "${repository}" "${commit}"
                    WORKING_DIRECTORY "${dir}" RESULT_VARIABLE result)
                if(result EQUAL 0)
                    break()
                endif()
            endforeach()
        endif()
        if(result EQUAL 0)
            execute_process(COMMAND "${GIT_EXECUTABLE}" -c advice.detachedHead=false checkout --quiet FETCH_HEAD
                WORKING_DIRECTORY "${dir}" RESULT_VARIABLE result)
        endif()
        if(NOT result EQUAL 0)
            file(REMOVE_RECURSE "${dir}")
            message(FATAL_ERROR "Could not fetch ${name} (${repository} at ${commit}). "
                "Check the network, or set ${override} to a local checkout.")
        endif()
        file(WRITE "${stamp}" "${commit}\n")
    endif()
    set(${out_var} "${dir}" PARENT_SCOPE)
endfunction()
