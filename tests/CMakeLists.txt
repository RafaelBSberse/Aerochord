find_package(GTest QUIET)

if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(googletest)
endif()

# ---------------------------------------------------------------------------
# Executável de testes
# ---------------------------------------------------------------------------
add_executable(aerochord_tests
    test_lock_free_queue.cpp
    test_gesture_analysis.cpp
    test_mapping.cpp
    test_pipeline_latency.cpp
)

target_link_libraries(aerochord_tests
    PRIVATE
        aerochord_common
        aerochord_gesture_analysis
        aerochord_mapping
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(aerochord_tests)
