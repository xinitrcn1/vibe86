# rtl/v1 testbenches: SystemVerilog unit tests, built with verilator --binary
# and run under ctest (only for the latest core version, rtl/CMakeLists).

function(v1_testbench name)
  # extra arguments: C sources (DPI-C) compiled into the testbench
  set(out ${CMAKE_CURRENT_BINARY_DIR}/${name})
  set(csrc)
  foreach(c IN LISTS ARGN)
    list(APPEND csrc ${CMAKE_CURRENT_SOURCE_DIR}/${c})
  endforeach()
  add_custom_command(OUTPUT ${out}/V${name}
    COMMAND verilator --binary --timing -j 0 ${CMAKE_CURRENT_SOURCE_DIR}/../v1.vlt
            -I${VIBE86_COMMON_DIR} --top-module ${name} --Mdir ${out} -o V${name}
            ${V1_RTL} ${CMAKE_CURRENT_SOURCE_DIR}/${name}.sv ${csrc}
    DEPENDS ${V1_RTL} ${CMAKE_CURRENT_SOURCE_DIR}/${name}.sv ${csrc} ${CMAKE_CURRENT_SOURCE_DIR}/../v1.vlt
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
