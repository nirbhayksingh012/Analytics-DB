cmake_minimum_required(VERSION 3.20)
project(SnowQuery VERSION 0.1.0 LANGUAGES CXX)

# ── C++20 standard ──────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Compiler warnings ──────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
elseif(MSVC)
    add_compile_options(/W4)
endif()

# ── Include paths ──────────────────────────────────────────────────
include_directories(${CMAKE_SOURCE_DIR}/include)

# ── Core library (all engine source files) ─────────────────────────
add_library(snowquery_core STATIC
    src/csv_reader.cpp
    src/table.cpp
    src/column_store.cpp
    src/metrics.cpp
)
target_include_directories(snowquery_core PUBLIC ${CMAKE_SOURCE_DIR}/include)

# ── Main executable ────────────────────────────────────────────────
add_executable(snowquery src/main.cpp)
target_link_libraries(snowquery PRIVATE snowquery_core)

# ── Tests ──────────────────────────────────────────────────────────
enable_testing()

add_executable(test_csv_reader tests/test_csv_reader.cpp)
target_link_libraries(test_csv_reader PRIVATE snowquery_core)
add_test(NAME CSVReaderTests COMMAND test_csv_reader)

add_executable(test_table tests/test_table.cpp)
target_link_libraries(test_table PRIVATE snowquery_core)
add_test(NAME TableTests COMMAND test_table)
