cmake_minimum_required(VERSION 3.20)

project(trading_simulator
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to an optimised build with debug info; an unset build type
# means no optimisation at all, which makes any timing meaningless.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(CHRONOS_SANITIZE "Build with AddressSanitizer + UBSan" OFF)

if(CHRONOS_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

set(CHRONOS_WARNINGS -Wall -Wextra -Wpedantic)


# -------------------------
# Simulator library
# -------------------------

add_library(simulator_lib

    src/orders/price_level.cpp
    src/orders/order_book.cpp
    src/orders/order.cpp
    src/orders/order_pool.cpp
    src/orders/order_index.cpp
    src/matching/matching_engine.cpp
    src/simulator/simulator.cpp
    src/replay/replay_engine.cpp
    src/replay/csv_replay_event_reader.cpp
    src/itch/byte_reader.cpp
    src/itch/itch_message_reader.cpp
    src/itch/itch_decoder.cpp
    src/itch/itch_normalizer.cpp
    src/itch/itch_replay_event_reader.cpp
    src/market/instrument_registry.cpp
    src/market/market_book.cpp
    src/io/mapped_file.cpp
    src/itch/itch_source.cpp
    src/market/market_replay.cpp
    src/cli/replay_args.cpp
    src/portfolio/portfolio.cpp
    src/sim/execution_simulator.cpp
)

target_include_directories(simulator_lib
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_options(simulator_lib PRIVATE ${CHRONOS_WARNINGS})


# -------------------------
# Application
# -------------------------

add_executable(simulator

    app/main.cpp
    app/alloc_counter.cpp
)

target_compile_options(simulator PRIVATE ${CHRONOS_WARNINGS})

# generated/build_info.hpp: the commit, dirty flag and build type for
# `replay --json`. Checked on every build, rewritten only when it changes.
add_custom_target(build_info
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
            -DOUTPUT=${PROJECT_BINARY_DIR}/generated/build_info.hpp
            -DBUILD_TYPE=${CMAKE_BUILD_TYPE}
            -P ${PROJECT_SOURCE_DIR}/cmake/build_info.cmake
    BYPRODUCTS ${PROJECT_BINARY_DIR}/generated/build_info.hpp
    VERBATIM
)

add_dependencies(simulator build_info)

target_include_directories(simulator PRIVATE ${PROJECT_BINARY_DIR}/generated)

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
    tests/order_index_test.cpp
    tests/order_pool_integration_test.cpp
    tests/simulator/simulator_test.cpp
    tests/simulator/simulator_matching_test.cpp
    tests/simulator/simulator_replay_test.cpp
    tests/replay/replay_engine_test.cpp
    tests/replay/replay_event_reader_test.cpp
    tests/itch/byte_reader_test.cpp
    tests/itch/itch_message_reader_test.cpp
    tests/itch/itch_decoder_test.cpp
    tests/itch/itch_normalizer_test.cpp
    tests/itch/itch_replay_event_reader_test.cpp
    tests/itch/itch_test_builder_test.cpp
    tests/replay/itch_replay_engine_test.cpp
    tests/market/market_event_test.cpp
    tests/market/market_event_source_test.cpp
    tests/market/instrument_registry_test.cpp
    tests/market/vector_source_test.cpp
    tests/market/market_book_test.cpp
    tests/source_conformance_test.cpp
    tests/io/mapped_file_test.cpp
    tests/itch/itch_source_test.cpp
    tests/market/market_replay_test.cpp
    tests/cli/replay_args_test.cpp
    tests/itch/itch_real_data_test.cpp
    tests/sim/sim_queue_test.cpp
    tests/sim/execution_simulator_test.cpp
    tests/portfolio/portfolio_test.cpp
    tests/sim/order_gateway_test.cpp
    tests/engine/event_loop_test.cpp
    tests/engine/backtest_determinism_test.cpp
    tests/strategies/imbalance_taker_test.cpp
    tests/strategies/market_maker_test.cpp
    tests/metrics/latency_histogram_test.cpp
)

target_compile_definitions(simulator_tests
    PRIVATE
        CHRONOS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
)

target_compile_options(simulator_tests PRIVATE ${CHRONOS_WARNINGS})

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

# Its own executable: the counting operator new must stay out of simulator_tests.
add_executable(alloc_counter_test

    tests/alloc_counter_test.cpp
    app/alloc_counter.cpp
)

target_compile_options(alloc_counter_test PRIVATE ${CHRONOS_WARNINGS})

target_include_directories(alloc_counter_test PRIVATE ${PROJECT_SOURCE_DIR}/app)

target_link_libraries(alloc_counter_test PRIVATE GTest::gtest_main)

gtest_discover_tests(alloc_counter_test)


# -------------------------
# Benchmarks: -DCHRONOS_BENCH=ON (bench/run.sh)
# -------------------------

option(CHRONOS_BENCH "Build the simulator_bench Google Benchmark target" OFF)

if(CHRONOS_BENCH)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        benchmark
        URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.zip
    )

    FetchContent_MakeAvailable(benchmark)

    add_executable(simulator_bench

        bench/simulator_bench.cpp
    )

    target_compile_options(simulator_bench PRIVATE ${CHRONOS_WARNINGS})

    # itch/itch_test_builder.hpp builds the decode benchmark's input.
    target_include_directories(simulator_bench PRIVATE ${PROJECT_SOURCE_DIR}/tests)

    target_link_libraries(simulator_bench
        PRIVATE
            simulator_lib
            benchmark::benchmark_main
    )
endif()