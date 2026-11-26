find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(veloplay_tests
    test_bytes.cpp
    test_http.cpp
    test_discovery.cpp
    test_crypto.cpp
    test_registration.cpp
    test_network.cpp
    test_input.cpp
    test_remote_control.cpp
    test_config.cpp
    test_stream_alerts.cpp
    test_keyboard_mouse.cpp
    test_app_state.cpp
    test_console_list.cpp
    test_app_controller.cpp
    test_strings.cpp
    test_latency.cpp
    test_update.cpp
    test_session.cpp
    test_media.cpp
    test_psn_account.cpp
    test_install.cpp
    test_snapshot.cpp
    test_recording.cpp
    test_capture.cpp
    test_discord.cpp
    test_wheel.cpp
)
target_link_libraries(veloplay_tests PRIVATE veloplay_core miniz::miniz veloplay_opus GTest::gtest GTest::gtest_main)
if(WIN32)
    target_sources(veloplay_tests PRIVATE test_platform_win.cpp test_record_win.cpp)
    target_link_libraries(veloplay_tests PRIVATE veloplay_platform)
endif()
if(VELOPLAY_LINUX_PLATFORM)
    # Tests of the Linux parts: one file per part, under tests/linux/.
    file(GLOB VELOPLAY_LINUX_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/linux/*.cpp")
    target_sources(veloplay_tests PRIVATE ${VELOPLAY_LINUX_TESTS})
    target_link_libraries(veloplay_tests PRIVATE veloplay_linux)
endif()
target_compile_definitions(veloplay_tests PRIVATE VELOPLAY_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_compile_definitions(veloplay_tests PRIVATE VELOPLAY_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

# PRE_TEST: the test list is read when ctest runs, not as a build step. On
# Windows the DLLs next to the executable are in place by then.
gtest_discover_tests(veloplay_tests DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 60
    TEST_FILTER "-ChiakiSession.*")
# These run the real session library against this machine and all need the
# console's fixed session port on loopback, so they must not run side by side.
gtest_discover_tests(veloplay_tests DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 60
    TEST_FILTER "ChiakiSession.*"
    PROPERTIES RESOURCE_LOCK veloplay_session_port)
