# rtl/v1 testbenches: SystemVerilog unit tests, built with verilator --binary
# and run under ctest (only for the latest core version, rtl/CMakeLists).

function(v1_testbench name)
  # extra arguments: C sources (DPI-C) compiled into the testbench, and
  # SystemVerilog units of rtl/v1 the die doesn't use yet (*.sv, before
  # the testbench: v1_x87_pkg)
  set(out ${CMAKE_CURRENT_BINARY_DIR}/${name})
  set(csrc)
  set(svsrc)
  foreach(c IN LISTS ARGN)
    if(c MATCHES "\\.sv$")
      list(APPEND svsrc ${CMAKE_CURRENT_SOURCE_DIR}/../${c})
    else()
      list(APPEND csrc ${CMAKE_CURRENT_SOURCE_DIR}/${c})
    endif()
  endforeach()
  add_custom_command(OUTPUT ${out}/V${name}
    COMMAND verilator --binary --timing -j 0 ${CMAKE_CURRENT_SOURCE_DIR}/../v1.vlt
            -I${VIBE86_COMMON_DIR} --top-module ${name} --Mdir ${out} -o V${name}
            ${V1_RTL} ${svsrc} ${CMAKE_CURRENT_SOURCE_DIR}/${name}.sv ${csrc}
    DEPENDS ${V1_RTL} ${svsrc} ${CMAKE_CURRENT_SOURCE_DIR}/${name}.sv ${csrc} ${CMAKE_CURRENT_SOURCE_DIR}/../v1.vlt
            ucode_v1
    COMMENT "testbench ${name}"
    VERBATIM)
  add_custom_target(tb-${name} ALL DEPENDS ${out}/V${name})
  add_test(NAME rtl/v1/${name} COMMAND ${out}/V${name})
endfunction()

v1_testbench(tb_alu)
v1_testbench(tb_fpu)

# against the host CPU's own instructions (ref_x86.c): x86-64 hosts only
if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "amd64|AMD64|x86_64")
  v1_testbench(tb_fpu_ref ref_x86.c)
  v1_testbench(tb_alu_ref ref_x86.c)
endif()

# v1's x87 unit (v1_x87_pkg) against the host's x87 (80-bit, FCW precision
# and rounding). The unit is work in progress and not in the core yet (v1
# decodes no x87 instructions): this test fails today (3247 of 102000
# checks: FPREM/FPREM1, FSCALE, FXTRACT, the transcendentals, FST m32/m64
# exception flags, the IE-over-DE priority), so it is built and run only
# with -DVIBE86_X87_WIP=ON, until the unit passes it and joins the core.
option(VIBE86_X87_WIP "build and run tb_x87_ref (the unfinished x87 unit)" OFF)
if(VIBE86_X87_WIP AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "amd64|AMD64|x86_64")
  v1_testbench(tb_x87_ref ref_x87.c v1_x87_pkg.sv)
endif()
