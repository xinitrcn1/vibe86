# vibe86: an x86-64 processor in SystemVerilog, its microcode toolchain and
# the server board it runs on (Verilator + a C board model).
#
#   cmake -S . -B build -G Ninja   (or Unix Makefiles)
#   cmake --build build -j
#   ctest --test-dir build -j      (rtl/<latest> testbenches + tests/)
cmake_minimum_required(VERSION 3.24)
project(vibe86 LANGUAGES C CXX)
cmake_policy(SET CMP0144 NEW)   # VERILATOR_ROOT is a hint for find_package(verilator)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
find_package(verilator 5 REQUIRED HINTS $ENV{VERILATOR_ROOT} /usr/local/share/verilator)

# Every core version under rtl/ is built (vsim -C selects one), but only the
# latest one's testbenches and the tests/ suite run under ctest: older
# versions are kept as references.
set(VIBE86_VERSIONS v1)
set(VIBE86_LATEST v1 CACHE STRING "core version the tests/ suite runs on")

enable_testing()

add_subdirectory(tools)
foreach(v IN LISTS VIBE86_VERSIONS)
  add_subdirectory(rtl/${v})
endforeach()
add_subdirectory(sim)
add_subdirectory(fw)
add_subdirectory(tests)
