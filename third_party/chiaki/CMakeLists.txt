# libchiaki from chiaki-ng: session setup, Takion transport, error correction
# and stream encryption of PlayStation Remote Play. See README.md in this
# directory for what is built, what is left out and how to update it.

include("${PROJECT_SOURCE_DIR}/cmake/FetchCommit.cmake")

set(VELOPLAY_CHIAKI_COMMIT 43728af2e639fecb8a7af1cd68c29820a40de973)
set(VELOPLAY_JERASURE_COMMIT 505ccb4bc69eee8d7ea40a6089a056b99671134f)
set(VELOPLAY_GF_COMPLETE_COMMIT fa54a4670a5705c84abf6c24b92b0cd479625478)
set(VELOPLAY_NANOPB_COMMIT 160d4f09e5fabb2b66aa2dea32d4f38ace2c4b3f) # nanopb-0.4.9.2

veloplay_fetch_commit(chiaki-ng https://github.com/streetpea/chiaki-ng.git ${VELOPLAY_CHIAKI_COMMIT} chiaki_dir)
veloplay_fetch_commit(jerasure https://github.com/streetpea/jerasure.git ${VELOPLAY_JERASURE_COMMIT} jerasure_dir)
veloplay_fetch_commit(gf-complete https://github.com/streetpea/gf-complete.git ${VELOPLAY_GF_COMPLETE_COMMIT} gf_dir)
veloplay_fetch_commit(nanopb https://github.com/nanopb/nanopb.git ${VELOPLAY_NANOPB_COMMIT} nanopb_dir)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# --- Galois field arithmetic and erasure coding (forward error correction) ---

add_library(veloplay_gf_complete STATIC
    "${gf_dir}/src/gf.c"
    "${gf_dir}/src/gf_wgen.c"
    "${gf_dir}/src/gf_w4.c"
    "${gf_dir}/src/gf_w8.c"
    "${gf_dir}/src/gf_w16.c"
    "${gf_dir}/src/gf_w32.c"
    "${gf_dir}/src/gf_w64.c"
    "${gf_dir}/src/gf_w128.c"
    "${gf_dir}/src/gf_rand.c"
    "${gf_dir}/src/gf_general.c"
    "${gf_dir}/src/gf_cpu.c")
target_include_directories(veloplay_gf_complete SYSTEM PUBLIC "${gf_dir}/include")
if(MSVC)
    # gf_cpu.c calls __cpuidex without including the header that declares it,
    # which clang rejects.
    target_compile_options(veloplay_gf_complete PRIVATE /FIintrin.h)
endif()

add_library(veloplay_jerasure STATIC
    "${jerasure_dir}/src/galois.c"
    "${jerasure_dir}/src/jerasure.c"
    "${jerasure_dir}/src/reed_sol.c"
    "${jerasure_dir}/src/cauchy.c"
    "${jerasure_dir}/src/liberation.c")
target_include_directories(veloplay_jerasure SYSTEM PUBLIC "${jerasure_dir}/include")
target_link_libraries(veloplay_jerasure PUBLIC veloplay_gf_complete)

# --- nanopb runtime for the Takion protobuf messages ---

add_library(veloplay_nanopb STATIC
    "${nanopb_dir}/pb_common.c"
    "${nanopb_dir}/pb_decode.c"
    "${nanopb_dir}/pb_encode.c")
target_include_directories(veloplay_nanopb SYSTEM PUBLIC "${nanopb_dir}")
# Same setting as chiaki-ng: the C11 static assert is not available everywhere.
target_compile_definitions(veloplay_nanopb PUBLIC PB_C99_STATIC_ASSERT)

# --- libchiaki, local-network subset ---

set(chiaki_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${chiaki_generated}/chiaki")

# No decoders inside the library: VeloPlay decodes video and audio itself.
# Upstream ships a config.h that switches Opus on, so this directory has to
# come first on the include path.
file(CONFIGURE OUTPUT "${chiaki_generated}/chiaki/config.h" CONTENT [=[
#ifndef CHIAKI_CONFIG_H
#define CHIAKI_CONFIG_H
#define CHIAKI_LIB_ENABLE_OPUS 0
#define CHIAKI_LIB_ENABLE_PI_DECODER 0
#define CHIAKI_LIB_ENABLE_FFMPEG_DECODER 0
#endif
]=] @ONLY)

# common.c is the one core file that reaches into the internet part of the
# library: chiaki_lib_init() also prepares the certificate cache used for
# connections through Sony's servers (aia.c, needs curl). That part is not
# built, so the two references are taken out of a copy. If upstream changes
# these lines the build stops here instead of producing something else.
file(READ "${chiaki_dir}/lib/src/common.c" common_c)
set(aia_include "#include \"aia.h\"\n")
set(aia_call "	ChiakiErrorCode aia_err = chiaki_aia_init();\n	if(aia_err != CHIAKI_ERR_SUCCESS)\n		return aia_err;\n")
string(FIND "${common_c}" "${aia_include}" aia_include_at)
string(FIND "${common_c}" "${aia_call}" aia_call_at)
if(aia_include_at EQUAL -1 OR aia_call_at EQUAL -1)
    message(FATAL_ERROR "lib/src/common.c of chiaki-ng no longer matches the expected text. "
        "Review third_party/chiaki/CMakeLists.txt against commit ${VELOPLAY_CHIAKI_COMMIT}.")
endif()
string(REPLACE "${aia_include}" "" common_c "${common_c}")
string(REPLACE "${aia_call}" "" common_c "${common_c}")
string(FIND "${common_c}" "aia" aia_left)
if(NOT aia_left EQUAL -1)
    message(FATAL_ERROR "lib/src/common.c of chiaki-ng has more references to aia than expected.")
endif()
set(common_patched "${chiaki_generated}/common_local.c")
if(EXISTS "${common_patched}")
    file(READ "${common_patched}" common_before)
endif()
if(NOT common_c STREQUAL "${common_before}")
    file(WRITE "${common_patched}" "${common_c}")
endif()

set(chiaki_units
    audio audioreceiver audiosender base64 bitstream congestioncontrol controller ctrl
    discovery discoveryservice ecdh fec feedback feedbacksender frameprocessor gkcrypt http
    launchspec log opusdecoder opusencoder orientation packetstats random regist reorderqueue
    rpcrypt senkusha session sock stoppipe streamconnection takion takionsendbuffer thread
    time videoreceiver)
set(chiaki_sources)
foreach(unit IN LISTS chiaki_units)
    list(APPEND chiaki_sources "${chiaki_dir}/lib/src/${unit}.c")
endforeach()

add_library(veloplay_chiaki STATIC
    ${chiaki_sources}
    "${chiaki_dir}/lib/src/remote/rudp.c"
    "${chiaki_dir}/lib/src/remote/rudpsendbuffer.c"
    "${common_patched}"
    generated/takion.pb.c
    holepunch_stub.c
    vp_session.c)
target_include_directories(veloplay_chiaki
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE
        "${chiaki_generated}"
        "${chiaki_dir}/lib/include"
        "${chiaki_dir}/lib/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated")
target_compile_definitions(veloplay_chiaki PRIVATE
    CHIAKI_VERSION_MAJOR=1 CHIAKI_VERSION_MINOR=10 CHIAKI_VERSION_PATCH=1 CHIAKI_VERSION="1.10.1")
target_link_libraries(veloplay_chiaki PRIVATE
    veloplay_jerasure veloplay_nanopb OpenSSL::Crypto Threads::Threads)
if(WIN32)
    target_link_libraries(veloplay_chiaki PRIVATE wsock32 ws2_32 bcrypt iphlpapi)
elseif(UNIX)
    target_link_libraries(veloplay_chiaki PRIVATE m)
endif()

set(chiaki_targets veloplay_gf_complete veloplay_jerasure veloplay_nanopb veloplay_chiaki)
foreach(target IN LISTS chiaki_targets)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS ON
        POSITION_INDEPENDENT_CODE ON
        FOLDER third_party)
    if(MSVC)
        # The sources use variable-length arrays, which Microsoft's compiler
        # does not support. chiaki-ng builds its Windows releases with clang-cl
        # for the same reason. The toolset ships with Visual Studio as the
        # component "C++ Clang tools for Windows" and produces objects that
        # link with the rest of the client.
        if(CMAKE_GENERATOR MATCHES "Visual Studio")
            set_target_properties(${target} PROPERTIES VS_PLATFORM_TOOLSET ClangCL)
        elseif(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
            message(FATAL_ERROR "libchiaki needs clang-cl on Windows. Use a Visual Studio generator "
                "(the ClangCL toolset is selected automatically) or set CMAKE_C_COMPILER=clang-cl.")
        endif()
        target_compile_options(${target} PRIVATE /w)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
    else()
        # Third-party code: its warnings are not ours to fix.
        target_compile_options(${target} PRIVATE -w)
    endif()
endforeach()
if(NOT MSVC AND NOT WIN32)
    target_compile_definitions(veloplay_chiaki PRIVATE _GNU_SOURCE)
endif()
