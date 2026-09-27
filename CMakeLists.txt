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