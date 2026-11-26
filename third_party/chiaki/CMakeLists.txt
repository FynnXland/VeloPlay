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

# The way through Sony's servers (chiaki-ng's remote/holepunch.c) needs curl
# with WebSocket support, json-c, libevent and miniupnpc. The Windows build
# gets them from vcpkg; elsewhere it stays off unless asked for, and stubs
# stand in (holepunch_stub.c).
if(WIN32)
    set(veloplay_psn_default ON)
else()
    set(veloplay_psn_default OFF)
endif()
option(VELOPLAY_PSN "Connect to the console through Sony's servers (needs curl, json-c, libevent, miniupnpc)"
    ${veloplay_psn_default})
if(VELOPLAY_PSN)
    find_package(CURL REQUIRED)
    find_package(PkgConfig QUIET)
    find_package(json-c CONFIG QUIET)
    if(TARGET json-c::json-c)
        set(veloplay_jsonc json-c::json-c)
    else()
        pkg_search_module(VP_JSONC REQUIRED json-c IMPORTED_TARGET)
        set(veloplay_jsonc PkgConfig::VP_JSONC)
    endif()
    find_package(Libevent CONFIG QUIET)
    if(TARGET libevent::core)
        set(veloplay_libevent libevent::core libevent::extra)
    else()
        pkg_search_module(VP_LIBEVENT REQUIRED libevent IMPORTED_TARGET)
        set(veloplay_libevent PkgConfig::VP_LIBEVENT)
    endif()
    find_package(miniupnpc CONFIG QUIET)
    if(TARGET miniupnpc::miniupnpc)
        set(veloplay_miniupnpc miniupnpc::miniupnpc)
    else()
        pkg_search_module(VP_MINIUPNPC miniupnpc IMPORTED_TARGET)
        if(VP_MINIUPNPC_FOUND)
            set(veloplay_miniupnpc PkgConfig::VP_MINIUPNPC)
        else()
            find_library(veloplay_miniupnpc NAMES miniupnpc REQUIRED)
        endif()
    endif()
endif()

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
if(VELOPLAY_PSN)
    # With the way through Sony's servers, aia.c is built and common.c stays as it is.
    set(common_patched "${chiaki_dir}/lib/src/common.c")
    set(common_c "")
endif()
if(EXISTS "${common_patched}")
    file(READ "${common_patched}" common_before)
endif()
if(NOT VELOPLAY_PSN AND NOT common_c STREQUAL "${common_before}")
    file(WRITE "${common_patched}" "${common_c}")
endif()

# holepunch.c passes two whole messages of Sony's servers to the logger as
# the format string itself: a '%' in them would read random memory. They go
# in as an argument instead (and vp_session.c keeps them out of the log). As
# above: if upstream changes the line, the build stops here.
set(chiaki_psn_sources)
if(VELOPLAY_PSN)
    file(READ "${chiaki_dir}/lib/src/remote/holepunch.c" holepunch_c)
    set(json_as_format "CHIAKI_LOGE(log, json_object_to_json_string_ext(payload_json, JSON_C_TO_STRING_PRETTY));")
    # Counted by length, not as a list: the text ends in a semicolon, CMake's list separator.
    string(LENGTH "${holepunch_c}" holepunch_length)
    string(REPLACE "${json_as_format}" "" holepunch_without "${holepunch_c}")
    string(LENGTH "${holepunch_without}" holepunch_without_length)
    string(LENGTH "${json_as_format}" json_as_format_length)
    math(EXPR json_as_format_count "(${holepunch_length} - ${holepunch_without_length}) / ${json_as_format_length}")
    if(NOT json_as_format_count EQUAL 2)
        message(FATAL_ERROR "lib/src/remote/holepunch.c of chiaki-ng no longer logs Sony's messages in exactly two places. "
            "Review third_party/chiaki/CMakeLists.txt against commit ${VELOPLAY_CHIAKI_COMMIT}.")
    endif()
    string(REPLACE "${json_as_format}"
        "CHIAKI_LOGE(log, \"%s\", json_object_to_json_string_ext(payload_json, JSON_C_TO_STRING_PRETTY));"
        holepunch_c "${holepunch_c}")

    set(aia_openssl_source "${chiaki_dir}/lib/src/aia_openssl.c")
    if(NOT WIN32)
        # The Linux release links a curl and OpenSSL built on another machine;
        # the certificate bundle compiled into them may not exist on this one
        # (Debian's path on Fedora or Arch). The program names the system's
        # bundle in VP_CA_BUNDLE before any thread starts (linuxos::caBundle()),
        # and every curl handle of holepunch.c takes it, as does the trust store
        # that checks recovered certificate chains. Windows never sets it and is
        # left as it is. Each anchor must occur exactly once, or the build stops.
        function(veloplay_replace_once text_var label old new)
            string(LENGTH "${${text_var}}" before_length)
            string(REPLACE "${old}" "" without "${${text_var}}")
            string(LENGTH "${without}" without_length)
            string(LENGTH "${old}" old_length)
            math(EXPR count "(${before_length} - ${without_length}) / ${old_length}")
            if(NOT count EQUAL 1)
                message(FATAL_ERROR "${label} of chiaki-ng no longer has the place for the certificate bundle "
                    "(found ${count} times). Review third_party/chiaki/CMakeLists.txt against commit "
                    "${VELOPLAY_CHIAKI_COMMIT}.")
            endif()
            string(REPLACE "${old}" "${new}" replaced "${${text_var}}")
            set(${text_var} "${replaced}" PARENT_SCOPE)
        endfunction()

        set(curl_include "#include <curl/curl.h>\n")
        veloplay_replace_once(holepunch_c "lib/src/remote/holepunch.c" "${curl_include}" "${curl_include}
/* VeloPlay: the certificate bundle of this system, named by the program (see third_party/chiaki/CMakeLists.txt). */
static const char *vp_ca_bundle(void)
{
\tconst char *bundle = getenv(\"VP_CA_BUNDLE\");
\treturn bundle && *bundle ? bundle : NULL;
}

static CURL *vp_curl_easy_init(void)
{
\tCURL *curl = curl_easy_init();
\tif(curl && vp_ca_bundle())
\t\tcurl_easy_setopt(curl, CURLOPT_CAINFO, vp_ca_bundle());
\treturn curl;
}
#define curl_easy_init vp_curl_easy_init
")
        # The chain repair of the WebSocket reads the bundle back to combine it with recovered certificates;
        # CURLINFO_CAINFO names the compiled-in one, not the one set above.
        set(ca_lookup "CURLcode ca_info_res = curl_easy_getinfo(curl, CURLINFO_CAINFO, &ca_path);")
        veloplay_replace_once(holepunch_c "lib/src/remote/holepunch.c" "${ca_lookup}"
            "${ca_lookup}\n                if(ca_info_res == CURLE_OK && vp_ca_bundle())\n                    ca_path = vp_ca_bundle();")

        file(READ "${aia_openssl_source}" aia_openssl_c)
        set(trust_lookup "curl_easy_getinfo(connection, CURLINFO_CAPATH, &path) != CURLE_OK)\n\t\tgoto error;\n")
        veloplay_replace_once(aia_openssl_c "lib/src/aia_openssl.c" "${trust_lookup}"
            "${trust_lookup}\tif(getenv(\"VP_CA_BUNDLE\") && *getenv(\"VP_CA_BUNDLE\"))\n\t\tfile = getenv(\"VP_CA_BUNDLE\");\n")
        # Compiled from a copy; its "aia.h" comes from lib/src, which is on the include path.
        set(aia_openssl_source "${chiaki_generated}/aia_openssl_local.c")
        if(EXISTS "${aia_openssl_source}")
            file(READ "${aia_openssl_source}" aia_openssl_before)
        endif()
        if(NOT aia_openssl_c STREQUAL "${aia_openssl_before}")
            file(WRITE "${aia_openssl_source}" "${aia_openssl_c}")
        endif()
    endif()

    set(holepunch_patched "${chiaki_generated}/holepunch_local.c")
    if(EXISTS "${holepunch_patched}")
        file(READ "${holepunch_patched}" holepunch_before)
    endif()
    if(NOT holepunch_c STREQUAL "${holepunch_before}")
        file(WRITE "${holepunch_patched}" "${holepunch_c}")
    endif()
    list(APPEND chiaki_psn_sources
        "${chiaki_dir}/lib/src/aia.c"
        "${aia_openssl_source}"
        "${holepunch_patched}")
else()
    list(APPEND chiaki_psn_sources holepunch_stub.c)
endif()

# streamconnection.c closes the stream's transport on its own when the console
# ends the stream, and destroys the transport's locks with it. Microphone
# packets are sent into that transport from a thread of ours, which could be
# in the middle of sending just then. So the library tells vp_session.c
# before it closes; vp_session.c stops sending under a lock of its own.
# chiaki-ng itself has no such guard. As with common.c: if upstream changes
# the line, the build stops here.
file(READ "${chiaki_dir}/lib/src/streamconnection.c" streamconnection_c)
set(takion_close "	chiaki_takion_close(&stream_connection->takion);\n")
string(FIND "${streamconnection_c}" "${takion_close}" takion_close_at)
string(FIND "${streamconnection_c}" "${takion_close}" takion_close_last REVERSE)
if(takion_close_at EQUAL -1 OR NOT takion_close_at EQUAL takion_close_last)
    message(FATAL_ERROR "lib/src/streamconnection.c of chiaki-ng no longer closes its transport in exactly one place. "
        "Review third_party/chiaki/CMakeLists.txt against commit ${VELOPLAY_CHIAKI_COMMIT}.")
endif()
string(REPLACE "${takion_close}" "	vp_stream_closing(stream_connection->session);\n${takion_close}"
    streamconnection_c "${streamconnection_c}")
set(streamconnection_c "struct chiaki_session_t;\nvoid vp_stream_closing(struct chiaki_session_t *session);\n${streamconnection_c}")
set(streamconnection_patched "${chiaki_generated}/streamconnection_local.c")
if(EXISTS "${streamconnection_patched}")
    file(READ "${streamconnection_patched}" streamconnection_before)
endif()
if(NOT streamconnection_c STREQUAL "${streamconnection_before}")
    file(WRITE "${streamconnection_patched}" "${streamconnection_c}")
endif()

set(chiaki_units
    audio audioreceiver audiosender base64 bitstream congestioncontrol controller ctrl
    discovery discoveryservice ecdh fec feedback feedbacksender frameprocessor gkcrypt http
    launchspec log opusdecoder opusencoder orientation packetstats random regist reorderqueue
    rpcrypt senkusha session sock stoppipe takion takionsendbuffer thread
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
    "${streamconnection_patched}"
    generated/takion.pb.c
    ${chiaki_psn_sources}
    vp_session.c)
target_include_directories(veloplay_chiaki
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE
        "${chiaki_generated}"
        "${chiaki_dir}/lib/include"
        "${chiaki_dir}/lib/src"
        # holepunch.c, compiled from a copy, includes "../aia.h" and "stun.h".
        "${chiaki_dir}/lib/src/remote"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated")
target_compile_definitions(veloplay_chiaki PRIVATE
    CHIAKI_VERSION_MAJOR=1 CHIAKI_VERSION_MINOR=10 CHIAKI_VERSION_PATCH=1 CHIAKI_VERSION="1.10.1")
target_link_libraries(veloplay_chiaki PRIVATE
    veloplay_jerasure veloplay_nanopb OpenSSL::Crypto Threads::Threads)
if(VELOPLAY_PSN)
    target_compile_definitions(veloplay_chiaki PRIVATE VP_WITH_PSN=1)
    target_link_libraries(veloplay_chiaki PRIVATE CURL::libcurl ${veloplay_jsonc} ${veloplay_libevent} ${veloplay_miniupnpc})
    if(WIN32)
        target_link_libraries(veloplay_chiaki PRIVATE crypt32)
    endif()
endif()
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
