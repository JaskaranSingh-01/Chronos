cmake_minimum_required(VERSION 3.20)

project(trading_simulator
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


# -------------------------
# Simulator library
# -------------------------

add_library(simulator_lib

    src/orders/price_level.cpp
    src/orders/order_book.cpp
    src/orders/order.cpp
    src/orders/order_pool.cpp
    src/matching/matching_engine.cpp
    src/simulator/simulator.cpp
    src/replay/replay_engine.cpp
    src/replay/replay_event_reader.cpp
    src/itch/byte_reader.cpp
    src/itch/itch_message_reader.cpp
    src/itch/itch_decoder.cpp
    src/itch/itch_normalizer.cpp
)

target_include_directories(simulator_lib
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)


# -------------------------
# Application
# -------------------------

add_executable(simulator

    app/main.cpp
)

target_link_libraries(simulator
    PRIVATE
        simulator_lib
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
    tests/order_test.cpp
    tests/price_level_test.cpp
    tests/order_book_test.cpp
    tests/matching_engine_test.cpp
    tests/order_pool_test.cpp
    tests/order_pool_integration_test.cpp
    tests/simulator_test.cpp
    tests/replay/replay_engine_test.cpp
    tests/replay/replay_event_reader_test.cpp
    tests/replay/replay_itch_lifecycle_test.cpp
    tests/itch/byte_reader_test.cpp
    tests/itch/itch_message_reader_test.cpp
    tests/itch/itch_decoder_test.cpp
    tests/itch/itch_normalizer_test.cpp
)

target_compile_definitions(simulator_tests
    PRIVATE
        CHRONOS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
)

target_link_libraries(simulator_tests
    PRIVATE
        simulator_lib
        GTest::gtest_main
)

target_include_directories(simulator_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

include(GoogleTest)

gtest_discover_tests(simulator_tests)