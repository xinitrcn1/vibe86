# rtl/v1 testbenches: SystemVerilog unit tests, built with verilator --binary
# and run under ctest (only for the latest core version, rtl/CMakeLists).

function(v1_testbench name)
  set(out ${CMAKE_CURRENT_BINARY_DIR}/${name})
  add_custom_command(OUTPUT ${out}/V${name}
    COMMAND verilator --binary --timing -Wno-fatal -Wno-WIDTH -Wno-UNUSED -j 0
            -I${VIBE86_COMMON_DIR} --top-module ${name} --Mdir ${out} -o V${name}
            ${V1_RTL} ${CMAKE_CURRENT_SOURCE_DIR}/${name}.sv
    DEPENDS ${V1_RTL} ${CMAKE_CURRENT_SOURCE_DIR}/${name}.sv ucode_v1
    COMMENT "testbench ${name}"
    VERBATIM)
  add_custom_target(tb-${name} ALL DEPENDS ${out}/V${name})
  add_test(NAME rtl/v1/${name} COMMAND ${out}/V${name})
endfunction()

v1_testbench(tb_alu)
v1_testbench(tb_fpu)
