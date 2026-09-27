cmake_minimum_required(VERSION 3.20)

project(trading_simulator
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(simulator
    app/main.cpp
    src/clock/clock.cpp
)

target_include_directories(simulator
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

# -------------------------
# Tests
# -------------------------

include(FetchContent)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.16.0.zip
)

FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(simulator_tests
    tests/price_test.cpp
    tests/quantity_test.cpp
    tests/clock_test.cpp
    tests/event_test.cpp
    tests/timestamp_test.cpp
    tests/enums_test.cpp
)

target_link_libraries(simulator_tests
    PRIVATE
        GTest::gtest_main
)

target_include_directories(simulator_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

include(GoogleTest)

gtest_discover_tests(simulator_tests)