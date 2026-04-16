cmake_minimum_required(VERSION 3.20)
project(rv_emulator VERSION 0.1.0 LANGUAGES CXX)

# ── Language Standard ────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)   # Force -std=c++20, not -std=gnu++20

# ── Compiler Flags ───────────────────────────────────────────────────────────
# In C++, the compiler is your last line of defense. Warnings are errors here.
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
)

# Release: full O3 optimisation + strip debug symbols
# Debug:   no optimisation + full debug info (so GDB can actually find your vars)
add_compile_options(
    "$<$<CONFIG:Release>:-O3>"
    "$<$<CONFIG:Release>:-DNDEBUG>"
    "$<$<CONFIG:Debug>:-O0>"
    "$<$<CONFIG:Debug>:-g3>"
)

# ── Source Files ─────────────────────────────────────────────────────────────
add_executable(rv_emulator
    src/main.cpp
    src/elf_loader.cpp
)

target_include_directories(rv_emulator PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# ── Default Build Type ───────────────────────────────────────────────────────
# Prevents accidental unoptimised production builds (a classic C++ gotcha).
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

message(STATUS "Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD}")
