# jtag: vsim's JTAG probe (OpenOCD remote_bitbang, vsim -D) end to end
# against the die's TAP (docs/jtag.md). Disabled until the die top drives
# the JTAG pins (rtl/<v>/<v>_die.sv, D-014): remove DISABLED then.
add_test(NAME jtag/bitbang
  COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/bitbang.py
          $<TARGET_FILE:vsim> ${PROJECT_SOURCE_DIR}/rtl/common/platform.toml)
set_tests_properties(jtag/bitbang PROPERTIES TIMEOUT 300 DISABLED TRUE)

# the same client against the TAP and debug module alone (tap_top.sv with
# stub core and fabric), driven by the probe server in vsim's per-cycle loop
add_executable(jtag_tap_harness tap_harness.cpp ${PROJECT_SOURCE_DIR}/sim/jtag_bitbang.cpp)
target_include_directories(jtag_tap_harness PRIVATE ${PROJECT_SOURCE_DIR}/sim)
verilate(jtag_tap_harness
  SOURCES ${VIBE86_COMMON_DIR}/vibe86_platform_pkg.sv ${VIBE86_JTAG_SV}
          ${CMAKE_CURRENT_SOURCE_DIR}/tap_top.sv
  TOP_MODULE tap_top
  PREFIX Vtap_top
  INCLUDE_DIRS ${VIBE86_COMMON_DIR}
  VERILATOR_ARGS -Wall)
add_test(NAME jtag/bitbang-tap
  COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/bitbang.py
          $<TARGET_FILE:jtag_tap_harness> ${PROJECT_SOURCE_DIR}/rtl/common/platform.toml --stub)
set_tests_properties(jtag/bitbang-tap PROPERTIES TIMEOUT 300)
