# rtl/v1: the first core (multi-cycle, microcoded) and its die (v1_die.sv:
# the core plus rtl/uncore). Its microcode and decoder tables come from
# tools/ucc --target v1.

vibe86_ucode(v1)

set(V1_RTL
  ${VIBE86_COMMON_SV}
  ${v1_UOP_PKG}
  ${v1_UCODE_PKG}
  ${CMAKE_CURRENT_SOURCE_DIR}/v1_fp_pkg.sv
  ${CMAKE_CURRENT_SOURCE_DIR}/v1_alu.sv
  ${CMAKE_CURRENT_SOURCE_DIR}/v1_vrf.sv
  ${CMAKE_CURRENT_SOURCE_DIR}/v1_core.sv
  ${VIBE86_UNCORE_SV}
  ${VIBE86_JTAG_SV}
  ${CMAKE_CURRENT_SOURCE_DIR}/v1_die.sv)
set_source_files_properties(${v1_UOP_PKG} ${v1_UCODE_PKG} PROPERTIES GENERATED TRUE)
set(V1_RTL ${V1_RTL} CACHE INTERNAL "")

# Verilator's multithreaded model (verilate THREADS): the die's partitions
# evaluate in parallel each cycle. The core is one large sequential
# partition, so the scheduler finds about two-way parallelism: 2 threads run
# a firmware boot ~1.3x faster than 1 (27.4 s -> 21.4 s for 5M cycles), 4
# no faster than 2 (and it warns, UNOPTTHREADS, waived below)
set(VIBE86_VERILATOR_THREADS 2 CACHE STRING "threads of the Verilated die model (1: single-threaded)")
if(VIBE86_VERILATOR_THREADS GREATER 1)
  set(V1_THREAD_ARGS THREADS ${VIBE86_VERILATOR_THREADS})
  set(V1_THREAD_WARN -Wno-UNOPTTHREADS)   # it splits what it can
endif()

add_library(vibe86_v1 STATIC)
add_dependencies(vibe86_v1 ucode_v1)
verilate(vibe86_v1
  SOURCES ${V1_RTL}
  TOP_MODULE v1_die
  PREFIX Vv1_die
  ${V1_THREAD_ARGS}
  INCLUDE_DIRS ${VIBE86_COMMON_DIR}
  VERILATOR_ARGS ${V1_DEBUG_ARGS} ${V1_THREAD_WARN} ${CMAKE_CURRENT_SOURCE_DIR}/v1.vlt -O3 --x-assign fast --x-initial fast
                 --output-split 20000)

# lint: verilator -Wall over the die must be clean (docs/synthesis.md);
# waivers that can't sit in the code are in v1.vlt
find_program(VERILATOR_EXE verilator REQUIRED HINTS $ENV{VERILATOR_ROOT}/bin)
if(VIBE86_LATEST STREQUAL "v1")
  add_test(NAME rtl/v1/lint
    COMMAND ${VERILATOR_EXE} -Wall --lint-only ${CMAKE_CURRENT_SOURCE_DIR}/v1.vlt
            -I${VIBE86_COMMON_DIR} --top-module v1_die ${V1_RTL})
endif()

# testbenches: only the latest version's run under ctest
if(VIBE86_LATEST STREQUAL "v1")
  add_subdirectory(tb)
endif()
