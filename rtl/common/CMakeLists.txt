# rtl/common: what every core version shares, generated from descriptions by
# tools/gen/rtlgen.py into the build tree (never into git):
#   cpuid.toml     CPUID identity and feature bits    -> vibe86_cpuid_pkg.sv
#   reset.toml     architectural reset state          -> vibe86_reset_pkg.sv,
#                                                         vibe86_reset_apply.svh
#   platform.toml  LAPIC/IOAPIC, map, ports, socket   -> vibe86_platform_pkg.sv,
#                  pin-group widths, MSR defaults        vibe86_platform.h
#   pins.toml      the die interface (external pins)  -> vibe86_die_pkg.sv,
#                                                         vibe86_die_ports.svh,
#                                                         vibe86_die.h (vsim)
# Generated at configure time (files are rewritten only when they change);
# editing a description or the generator re-runs CMake.

set(_desc ${CMAKE_CURRENT_SOURCE_DIR})
set(_gen ${CMAKE_BINARY_DIR}/rtl/common)
set(_tool ${PROJECT_SOURCE_DIR}/tools/gen/rtlgen.py)
string(REPLACE ";" "," _versions "${VIBE86_VERSIONS}")
execute_process(
  COMMAND ${Python3_EXECUTABLE} -B ${_tool} --desc ${_desc} --out ${_gen} --versions ${_versions}
  RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "rtlgen failed for rtl/common")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${_tool} ${_desc}/cpuid.toml ${_desc}/reset.toml ${_desc}/platform.toml ${_desc}/pins.toml)

set(VIBE86_COMMON_DIR ${_gen} CACHE INTERNAL "generated rtl/common (SV packages, includes, C headers)")
set(VIBE86_COMMON_SV
  ${_gen}/vibe86_platform_pkg.sv
  ${_gen}/vibe86_die_pkg.sv
  ${_gen}/vibe86_cpuid_pkg.sv
  ${_gen}/vibe86_reset_pkg.sv
  CACHE INTERNAL "")
set(VIBE86_PLATFORM_TOML ${_desc}/platform.toml CACHE INTERNAL "")
