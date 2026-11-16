# rtl/v1: the first core (multi-cycle, microcoded). Its microcode and
# decoder tables come from tools/ucc --target v1.

vibe86_ucode(v1)

set(V1_RTL
  ${v1_UOP_PKG}
  ${v1_UCODE_PKG}
  ${CMAKE_CURRENT_SOURCE_DIR}/v1_alu.sv
  ${CMAKE_CURRENT_SOURCE_DIR}/v1_core.sv)
set_source_files_properties(${v1_UOP_PKG} ${v1_UCODE_PKG} PROPERTIES GENERATED TRUE)
set(V1_RTL ${V1_RTL} CACHE INTERNAL "")

add_library(vibe86_v1 STATIC)
add_dependencies(vibe86_v1 ucode_v1)
verilate(vibe86_v1
  SOURCES ${V1_RTL}
  TOP_MODULE v1_core
  PREFIX Vv1_core
  VERILATOR_ARGS -Wno-fatal -Wno-WIDTH -Wno-UNUSED -O3 --x-assign fast --x-initial fast
                 --output-split 20000)

# testbenches: only the latest version's run under ctest
if(VIBE86_LATEST STREQUAL "v1")
  add_subdirectory(tb)
endif()
