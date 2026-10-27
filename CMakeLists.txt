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
    src/orders/level_pool.cpp
    src/itch/byte_reader.cpp
    src/itch/itch_message_reader.cpp
    src/itch/itch_decoder.cpp
    src/market/instrument_registry.cpp
    src/market/market_book.cpp
    src/io/mapped_file.cpp
    src/csv/csv_source.cpp
    src/itch/itch_source.cpp
    src/market/market_replay.cpp
    src/market/pipeline_source.cpp
    src/market/trade.cpp
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
# The per-symbol store (include/simulator/store): each symbol's days as zstd frames, and its index
# -------------------------

include(FetchContent)

# zstd (BSD-3), pinned: built static from its own CMake files, without its programs, tests, legacy formats, threads or
# dictionary builder.
FetchContent_Declare(
    zstd
    URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
    URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
    SOURCE_SUBDIR build/cmake
)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_DICTBUILDER OFF CACHE BOOL "" FORCE)

# xxHash (BSD-2), header-only and pinned, downloaded only: the XXH3-128 that identifies a day and each segment.
FetchContent_Declare(
    xxhash
    URL https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.3.tar.gz
    URL_HASH SHA256=aae608dfe8213dfd05d909a57718ef82f30722c392344583d3f39050c7f29a80
    SOURCE_SUBDIR none
)

FetchContent_MakeAvailable(zstd xxhash)

add_library(simulator_store

    src/store/store.cpp
    src/store/store_index.cpp
)

target_compile_options(simulator_store PRIVATE ${CHRONOS_WARNINGS})

target_link_libraries(simulator_store PUBLIC simulator_lib PRIVATE libzstd_static)
target_include_directories(simulator_store SYSTEM PRIVATE ${zstd_SOURCE_DIR}/lib ${xxhash_SOURCE_DIR})


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
# `replay --json`, and a Lab run's provenance. Checked on every build,
# rewritten only when it changes.
add_custom_target(build_info
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
            -DOUTPUT=${PROJECT_BINARY_DIR}/generated/build_info.hpp
            -DBUILD_TYPE=${CMAKE_BUILD_TYPE}
            "-DCOMPILER=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
            -DSANITIZE=${CHRONOS_SANITIZE}
            "-DPLATFORM=${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}"
            -P ${PROJECT_SOURCE_DIR}/cmake/build_info.cmake
    BYPRODUCTS ${PROJECT_BINARY_DIR}/generated/build_info.hpp
    VERBATIM
)

add_dependencies(simulator build_info)

target_include_directories(simulator PRIVATE ${PROJECT_BINARY_DIR}/generated)

target_link_libraries(simulator
    PRIVATE
        simulator_store
)


# -------------------------
# Server library: the interactive session. The engine never depends on it (scripts/check-isolation.sh).
# -------------------------

find_package(Threads REQUIRED)

FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)

FetchContent_MakeAvailable(json)

# Crow (BSD-3) over standalone Asio for HTTP and WebSocket: header-only and pinned. Their own CMake files are not run:
# a SOURCE_SUBDIR with no CMakeLists.txt makes FetchContent download only. SYSTEM keeps their warnings out of ours.
FetchContent_Declare(
    asio
    URL https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-30-2.tar.gz
    URL_HASH SHA256=755bd7f85a4b269c67ae0ea254907c078d408cce8e1a352ad2ed664d233780e8
    SOURCE_SUBDIR none
)

FetchContent_Declare(
    crow
    URL https://github.com/CrowCpp/Crow/archive/refs/tags/v1.3.4.tar.gz
    URL_HASH SHA256=6be759b3a648b41f0adc3a8408a53fd0243775217b4e32e8101d69e4aadf48cc
    SOURCE_SUBDIR none
    # Fixes Crow 1.3.4 lacks: overflow-safe max_payload, a request-body cap, pong only when idle; a request's flags
    # initialised, as its parser's clear() copies a fresh request (UBSan: an indeterminate bool); and an atomic shutdown
    # flag, which stop() writes while the acceptor reads it (TSan).
    PATCH_COMMAND patch -p1 -i ${PROJECT_SOURCE_DIR}/cmake/crow-1.3.4.patch
)

FetchContent_MakeAvailable(asio crow)

add_library(crow_headers INTERFACE)
target_include_directories(crow_headers SYSTEM INTERFACE ${crow_SOURCE_DIR}/include ${asio_SOURCE_DIR}/asio/include)
# CROW_DISABLE_STATIC_DIR: no implicit /static/ route serving the working directory.
target_compile_definitions(crow_headers INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED CROW_DISABLE_STATIC_DIR)

add_library(chronos_server

    server/src/backtest.cpp
    server/src/candles.cpp
    server/src/command_log.cpp
    server/src/generation.cpp
    server/src/http_server.cpp
    server/src/itch_input.cpp
    server/src/lab.cpp
    server/src/peer.cpp
    server/src/protocol.cpp
    server/src/replay_session.cpp
    server/src/security.cpp
    server/src/session_runner.cpp
)

target_include_directories(chronos_server
    PUBLIC
        ${PROJECT_SOURCE_DIR}/server/include
)

target_compile_options(chronos_server PRIVATE ${CHRONOS_WARNINGS})

target_link_libraries(chronos_server
    PUBLIC
        simulator_store
        Threads::Threads
)

target_link_libraries(chronos_server PRIVATE nlohmann_json::nlohmann_json crow_headers)
target_include_directories(chronos_server SYSTEM PRIVATE ${xxhash_SOURCE_DIR})

# The command log's header records the commit (generated/build_info.hpp).
add_dependencies(chronos_server build_info)
target_include_directories(chronos_server PRIVATE ${PROJECT_BINARY_DIR}/generated)

# The interactive server; its main lives under server/, never app/ (scripts/check-isolation.sh).
add_executable(chronos-serve

    server/src/main.cpp
)

target_compile_options(chronos-serve PRIVATE ${CHRONOS_WARNINGS})

target_link_libraries(chronos-serve PRIVATE chronos_server)


# -------------------------
# Tests
# -------------------------

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
    tests/hash_index_test.cpp
    tests/level_index_test.cpp
    tests/level_pool_test.cpp
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
    tests/market/book_model_test.cpp
    tests/market/ladder_test.cpp
    tests/market/spsc_ring_test.cpp
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
    tests/strategies/orb_test.cpp
    tests/strategies/market_maker_test.cpp
    tests/metrics/latency_histogram_test.cpp
    tests/store/store_index_test.cpp
    tests/store/store_test.cpp
)

target_compile_definitions(simulator_tests
    PRIVATE
        CHRONOS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
)

target_compile_options(simulator_tests PRIVATE ${CHRONOS_WARNINGS})

target_link_libraries(simulator_tests
    PRIVATE
        simulator_store
        GTest::gtest_main
)

target_include_directories(simulator_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/tests
)

include(GoogleTest)

gtest_discover_tests(simulator_tests)

# Its own executable, so simulator_tests never links server code.
add_executable(chronos_server_tests

    tests/server/backtest_test.cpp
    tests/server/candles_test.cpp
    tests/server/channels_test.cpp
    tests/server/command_log_test.cpp
    tests/server/generation_test.cpp
    tests/server/itch_input_test.cpp
    tests/server/lab_test.cpp
    tests/server/peer_test.cpp
    tests/server/protocol_test.cpp
    tests/server/replay_session_test.cpp
    tests/server/restart_test.cpp
    tests/server/security_test.cpp
    tests/server/server_test.cpp
    tests/server/session_runner_test.cpp
    tests/server/snapshot_test.cpp
    tests/server/watch_test.cpp
)

target_compile_options(chronos_server_tests PRIVATE ${CHRONOS_WARNINGS})

# itch/itch_test_builder.hpp builds ITCH input.
target_include_directories(chronos_server_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)

target_link_libraries(chronos_server_tests
    PRIVATE
        chronos_server
        nlohmann_json::nlohmann_json
        GTest::gtest_main
)

gtest_discover_tests(chronos_server_tests)

# Writes the small ITCH day the web end-to-end tests serve (web/e2e).
add_executable(chronos-e2e-fixture

    tests/e2e/itch_fixture.cpp
)

target_compile_options(chronos-e2e-fixture PRIVATE ${CHRONOS_WARNINGS})

target_include_directories(chronos-e2e-fixture PRIVATE ${PROJECT_SOURCE_DIR}/tests)

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

# The allocation count is read immediately around MarketReplay::run: opening the file and building the
# source, the book and the report all allocate, and a run() that allocates nothing still counts 0. With --threads 2 the
# decoder's thread and ring are made before it.
add_test(NAME SimulatorCliTest.TheAllocationCountCoversOnlyTheReplay
         COMMAND sh -c "for threads in 1 2; do
                            rm -f \"$1\"
                            \"$0\" replay /dev/null --symbol AAPL --date 2019-12-30 --threads $threads --json \"$1\" > /dev/null
                            grep -q '\"allocations\": 0,' \"$1\" || exit 1
                        done"
                 $<TARGET_FILE:simulator> ${PROJECT_BINARY_DIR}/allocation-interval.json)

# A full book stops the replay and says what is full: ACME's day adds two orders, and --capacity holds one.
add_test(NAME SimulatorCliTest.AFullOrderPoolStopsTheReplayAndSaysSo
         COMMAND sh -c "dir=$(mktemp -d) && \"$1\" \"$dir\" || exit 1
                        out=$(\"$0\" replay \"$dir/12302019.NASDAQ_ITCH50.bin\" --symbol ACME --date 2019-12-30 --capacity 1)
                        code=$?; rm -rf \"$dir\"
                        test $code -eq 1 && echo \"$out\" | grep -qF 'hint: the order pool (1) is full; raise --capacity'"
                 $<TARGET_FILE:simulator> $<TARGET_FILE:chronos-e2e-fixture>)

# A replay allocates nothing once it starts: no allocation counted, and the heap unchanged, in a replay of
# every symbol of the Lab's Monday (ACME, ZETA and IDLE) and of one of them, with either backend (the ladder's windows
# reserved), on one thread or two. ASan builds count nothing (its allocator owns the heap), so the Release, UBSan and
# macOS jobs enforce this.
add_test(NAME SimulatorCliTest.AReplayAllocatesNothing
         COMMAND sh -c "dir=$(mktemp -d) && \"$1\" \"$dir\" --lab || exit 1
                        for symbols in --all '--symbol ACME'; do for book in list ladder; do for threads in 1 2; do
                            \"$0\" replay \"$dir/12302019.NASDAQ_ITCH50.bin\" $symbols --date 2019-12-30 --book $book --threads $threads --json \"$dir/run.json\" > /dev/null &&
                                grep -q '\"allocations\": 0,' \"$dir/run.json\" &&
                                grep -q '\"heap_blocks_change\": 0, \"heap_bytes_change\": 0,' \"$dir/run.json\" &&
                                { test $book = list || ! grep -q '\"windows_reserved_bytes\": 0,' \"$dir/run.json\"; } ||
                                { rm -rf \"$dir\"; exit 1; }
                        done; done; done; rm -rf \"$dir\""
                 $<TARGET_FILE:simulator> $<TARGET_FILE:chronos-e2e-fixture>)

# --store: a symbol the store lacks on the day, or an index that is invalid, exits 2 and says which.
set(empty_store ${PROJECT_BINARY_DIR}/empty-store)
set(bad_store ${PROJECT_BINARY_DIR}/bad-store)
file(MAKE_DIRECTORY ${empty_store})
file(WRITE ${bad_store}/index.tsv "not an index\n")
function(chronos_cli_message_test name message)
    add_test(NAME SimulatorCliTest.${name}
             COMMAND sh -c "out=$(\"$0\" \"$@\" 2>&1); test $? -eq 2 && echo \"$out\" | grep -qF '${message}'"
                     $<TARGET_FILE:simulator> ${ARGN})
endfunction()

chronos_cli_message_test(StoreWithoutTheSymbolExitsTwo "no AAPL on 2019-12-30 in ${empty_store}"
                         replay --store ${empty_store} --symbol AAPL --date 2019-12-30)
chronos_cli_message_test(InvalidStoreIndexExitsTwo "the store index at ${bad_store} is invalid; chronos-serve rebuilds it"
                         backtest --store ${bad_store} --symbol AAPL --date 2019-12-30)

# chronos-serve's usage exit codes; serving itself is ServerTest's.
function(chronos_serve_test name exit_code)
    add_test(NAME ChronosServeCliTest.${name}
             COMMAND sh -c "\"$0\" \"$@\"; test $? -eq ${exit_code}" $<TARGET_FILE:chronos-serve> ${ARGN})
endfunction()

chronos_serve_test(HelpExitsZero 0 --help)
chronos_serve_test(BadOptionExitsTwo 2 --data . --date 2019-12-30 --frobnicate)
chronos_serve_test(NoDayFileExitsTwo 2 --data .)
chronos_serve_test(BadPortExitsTwo 2 --data . --date 2019-12-30 --port 70000)
chronos_serve_test(BadBookExitsTwo 2 --data . --date 2019-12-30 --book tree)
chronos_serve_test(MissingFileExitsTwo 2 --data /nonexistent/chronos-data --date 2019-12-30)

# Other failures exit 2 too, so these check the message. Without --date the latest day starts, here the only one; its
# hash goes to runs/ in the fixture directory, the tests' working directory. Each test has its own store, as a store is
# one chronos-serve's at a time and ctest -j runs them together.
set(serve_fixture ${PROJECT_BINARY_DIR}/chronos-serve-fixture)
file(WRITE ${serve_fixture}/12302019.NASDAQ_ITCH50.bin "not itch")
function(chronos_serve_message_test name message)
    add_test(NAME ChronosServeCliTest.${name}
             COMMAND sh -c "out=$(\"$0\" \"$@\" 2>&1); test $? -eq 2 && echo \"$out\" | grep -q '${message}'"
                     $<TARGET_FILE:chronos-serve> --data ${serve_fixture} --store ${serve_fixture}/store-${name} ${ARGN}
             WORKING_DIRECTORY ${serve_fixture})
    set_tests_properties(ChronosServeCliTest.${name} PROPERTIES TIMEOUT 20)  # serving would never exit
endfunction()

chronos_serve_message_test(NoMarketEventsExitsTwo ": no symbol has market events")
chronos_serve_message_test(DateWithoutADayFileExitsTwo "no day file for 2019-12-27" --date 2019-12-27)

# The store is one chronos-serve's, and simulator reads it (tests/e2e/store_serve_test.sh).
add_test(NAME ChronosServeCliTest.AStoreIsOneServersAndTheCliReplaysFromIt
         COMMAND sh ${PROJECT_SOURCE_DIR}/tests/e2e/store_serve_test.sh $<TARGET_FILE:chronos-serve>
                 $<TARGET_FILE:simulator> $<TARGET_FILE:chronos-e2e-fixture>)
set_tests_properties(ChronosServeCliTest.AStoreIsOneServersAndTheCliReplaysFromIt PROPERTIES TIMEOUT 60)

# build_info.cmake's source digest tells two trees at one commit apart (needs git).
add_test(NAME BuildInfoTest.DirtyTreesAtOneCommitGetTheirOwnSourceDigest
         COMMAND ${CMAKE_COMMAND} -DBUILD_INFO=${PROJECT_SOURCE_DIR}/cmake/build_info.cmake
                 -DWORK=${PROJECT_BINARY_DIR}/build-info-test -P ${PROJECT_SOURCE_DIR}/tests/build_info_test.cmake)


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