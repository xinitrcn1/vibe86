# rtl/uncore: the parts of the die every core version shares (D-014;
# docs/die-interface.md). Each version's die top lists the sources it uses.
#
# The memory system (D-014): DDR5 subchannel controllers, the init engine,
# the address fabric (with the I/O link endpoint and the host bridge)
set(VIBE86_UNCORE_SV
  ${CMAKE_CURRENT_SOURCE_DIR}/ddr5_pkg.sv
  ${CMAKE_CURRENT_SOURCE_DIR}/ddr5_sub.sv
  ${CMAKE_CURRENT_SOURCE_DIR}/meminit.sv
  ${CMAKE_CURRENT_SOURCE_DIR}/fabric.sv
  CACHE INTERNAL "")

# JTAG (docs/jtag.md): the TAP (TCK domain) and the debug module (core domain)
set(VIBE86_JTAG_SV
  ${CMAKE_CURRENT_SOURCE_DIR}/jtag_tap.sv
  ${CMAKE_CURRENT_SOURCE_DIR}/jtag_dm.sv
  CACHE INTERNAL "")

find_program(VERILATOR_EXE verilator REQUIRED HINTS $ENV{VERILATOR_ROOT}/bin)

# lint: verilator -Wall must be clean
add_test(NAME rtl/uncore/lint-jtag
  COMMAND ${VERILATOR_EXE} -Wall --lint-only -I${VIBE86_COMMON_DIR} --top-module jtag_tap
          ${VIBE86_COMMON_DIR}/vibe86_platform_pkg.sv ${VIBE86_JTAG_SV})
add_test(NAME rtl/uncore/lint-jtag-dm
  COMMAND ${VERILATOR_EXE} -Wall --lint-only --top-module jtag_dm
          ${CMAKE_CURRENT_SOURCE_DIR}/jtag_dm.sv)

# testbenches (verilator --binary)
function(uncore_testbench name)
  set(out ${CMAKE_CURRENT_BINARY_DIR}/${name})
  add_custom_command(OUTPUT ${out}/V${name}
    COMMAND ${VERILATOR_EXE} --binary --timing --no-sched-zero-delay -j 0
            -I${VIBE86_COMMON_DIR} --top-module ${name} --Mdir ${out} -o V${name}
            ${VIBE86_COMMON_DIR}/vibe86_platform_pkg.sv ${ARGN}
            ${CMAKE_CURRENT_SOURCE_DIR}/tb/${name}.sv
    DEPENDS ${ARGN} ${CMAKE_CURRENT_SOURCE_DIR}/tb/${name}.sv ${VIBE86_COMMON_DIR}/vibe86_platform_pkg.sv
    COMMENT "testbench ${name}"
    VERBATIM)
  add_custom_target(tb-${name} ALL DEPENDS ${out}/V${name})
  add_test(NAME rtl/uncore/${name} COMMAND ${out}/V${name})
endfunction()

uncore_testbench(tb_jtag ${VIBE86_JTAG_SV})
