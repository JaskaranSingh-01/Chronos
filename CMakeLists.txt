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

# OFF, ON (= address,undefined) or a -fsanitize= list such as undefined, the
# choice where ASan does not work (it hangs at startup on macOS 27 with Apple
# clang 17). Any report aborts, so a passing test is a clean one.
set(CHRONOS_SANITIZE OFF CACHE STRING "OFF, ON (= address,undefined) or a -fsanitize= list")

if(CHRONOS_SANITIZE)
    set(chronos_sanitizers "${CHRONOS_SANITIZE}")
    if("${CHRONOS_SANITIZE}")  # quoted: true only for ON, YES, TRUE, Y or a non-zero number
        set(chronos_sanitizers address,undefined)
    endif()
    add_compile_options(-fsanitize=${chronos_sanitizers} -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${chronos_sanitizers})
endif()

# ASan replaces operator new itself, so the allocation counter's replacement would clash with it.
if(chronos_sanitizers MATCHES "address")
    set(CHRONOS_COUNT_ALLOCATIONS OFF)
else()
    set(CHRONOS_COUNT_ALLOCATIONS ON)
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
    src/itch/byte_reader.cpp
    src/itch/itch_message_reader.cpp
    src/itch/itch_decoder.cpp
    src/market/instrument_registry.cpp
    src/market/market_book.cpp
    src/io/mapped_file.cpp
    src/csv/csv_source.cpp
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

if(NOT CHRONOS_COUNT_ALLOCATIONS)
    target_compile_definitions(simulator PRIVATE CHRONOS_NO_ALLOC_COUNTER)
endif()

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
# Server library: the interactive session. The engine never depends on it (scripts/check-isolation.sh).
# -------------------------

find_package(Threads REQUIRED)

add_library(chronos_server

    server/src/candles.cpp
)

target_include_directories(chronos_server
    PUBLIC
        ${PROJECT_SOURCE_DIR}/server/include
)

target_compile_options(chronos_server PRIVATE ${CHRONOS_WARNINGS})

target_link_libraries(chronos_server
    PUBLIC
        simulator_lib
        Threads::Threads
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
    tests/timestamp_test.cpp
    tests/enums_test.cpp
    tests/order_test.cpp
    tests/price_level_test.cpp
    tests/order_book_test.cpp
    tests/order_pool_test.cpp
    tests/order_index_test.cpp
    tests/order_pool_integration_test.cpp
    tests/itch/byte_reader_test.cpp
    tests/itch/itch_message_reader_test.cpp
    tests/itch/itch_decoder_test.cpp
    tests/itch/itch_test_builder_test.cpp
    tests/market/market_event_test.cpp
    tests/market/market_event_source_test.cpp
    tests/market/instrument_registry_test.cpp
    tests/market/vector_source_test.cpp
    tests/market/market_book_test.cpp
    tests/source_conformance_test.cpp
    tests/io/mapped_file_test.cpp
    tests/csv/csv_source_test.cpp
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

# Its own executable, so simulator_tests never links server code.
add_executable(chronos_server_tests

    tests/server/candles_test.cpp
    tests/server/channels_test.cpp
)

target_compile_options(chronos_server_tests PRIVATE ${CHRONOS_WARNINGS})

target_link_libraries(chronos_server_tests
    PRIVATE
        chronos_server
        GTest::gtest_main
)

gtest_discover_tests(chronos_server_tests)

# Its own executable: the counting operator new must stay out of simulator_tests.
if(CHRONOS_COUNT_ALLOCATIONS)
    add_executable(alloc_counter_test

        tests/alloc_counter_test.cpp
        app/alloc_counter.cpp
    )

    target_compile_options(alloc_counter_test PRIVATE ${CHRONOS_WARNINGS})

    target_include_directories(alloc_counter_test PRIVATE ${PROJECT_SOURCE_DIR}/app)

    target_link_libraries(alloc_counter_test PRIVATE GTest::gtest_main)

    gtest_discover_tests(alloc_counter_test)
endif()

# main()'s exit codes as `simulator --help` documents them. CTest alone only
# tells zero from non-zero, so sh compares the code.
function(chronos_cli_test name exit_code)
    add_test(NAME SimulatorCliTest.${name}
             COMMAND sh -c "\"$0\" \"$@\"; test $? -eq ${exit_code}" $<TARGET_FILE:simulator> ${ARGN})
endfunction()

chronos_cli_test(HelpExitsZero 0 --help)
chronos_cli_test(VersionExitsZero 0 --version)
chronos_cli_test(BadOptionExitsTwo 2 replay day.bin --symbol AAPL --date 2019-12-30 --frobnicate)
chronos_cli_test(MissingFileExitsTwo 2 replay /nonexistent/chronos-missing.itch --symbol AAPL --date 2019-12-30)
chronos_cli_test(FailedRunExitsOne 1 replay /dev/null --symbol AAPL --date 2019-12-30)


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