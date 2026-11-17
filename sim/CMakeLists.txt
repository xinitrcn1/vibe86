# vsim: the Verilated core (rtl/<version>, vsim -C) + the board model + an
# SDL3 window for the VGA "monitor".
#
# Each core version's rtl/<v>/CMakeLists.txt makes a static library target
# vibe86_<v> with verilate(... PREFIX V<v>_core TOP_MODULE <v>_core); its
# include directories come with the target. This file adds the adapter
# cpu_<v>.cpp for each version in VIBE86_VERSIONS.
#
# Note for the multi-version case: verilate() adds the Verilator runtime
# (verilated.cpp, verilated_threads.cpp, ...) to *every* target it is called
# on. With static libraries the linker pulls only the archive members it
# needs, so a second copy is normally ignored, but it is fragile. When a
# second version lands, build the runtime once (a vibe86_vlrt library from
# ${VERILATOR_ROOT}/include/verilated*.cpp) and verilate the versions with
# their *_GLOBAL sources removed, or Verilate all versions in one run.

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)

# libtpms: the debug TPM 2.0 behind the I2C controller (tpm.c)
find_path(TPMS_INCLUDE_DIR libtpms/tpm_library.h HINTS /usr/local/include REQUIRED)
find_library(TPMS_LIBRARY tpms HINTS /usr/local/lib REQUIRED)

set(VSIM_WARN -Wall -Wextra)

# the board model (C11): everything outside the core
# tpm_debug_state.h embeds tpm-debug.permall (the debug TPM's fixed state)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/tpm_debug_state.h
  COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/tpm-debug.permall
          -DOUT=${CMAKE_CURRENT_BINARY_DIR}/tpm_debug_state.h -DNAME=tpm_debug_permall
          -P ${CMAKE_CURRENT_SOURCE_DIR}/bin2h.cmake
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tpm-debug.permall ${CMAKE_CURRENT_SOURCE_DIR}/bin2h.cmake
  COMMENT "tpm_debug_state.h from tpm-debug.permall"
  VERBATIM)

add_library(vsim_board STATIC
  board.c iod.c vr.c ioapic.c spd.c ddr5.c vga.c pmc.c i2c.c tpm.c pcie.c iommu.c xhci.c pc.c
  ${CMAKE_CURRENT_BINARY_DIR}/tpm_debug_state.h)
target_include_directories(vsim_board PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${VIBE86_COMMON_DIR}
  PRIVATE ${TPMS_INCLUDE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_compile_options(vsim_board PRIVATE ${VSIM_WARN})
target_link_libraries(vsim_board PUBLIC ${TPMS_LIBRARY})

add_executable(vsim vsim.cpp cpu.cpp jtag_bitbang.cpp)
target_compile_options(vsim PRIVATE ${VSIM_WARN})
foreach(v IN LISTS VIBE86_VERSIONS)
  # the Verilated model's and Verilator's headers: system headers to us, so
  # our -Wall -Wextra doesn't report warnings in them
  get_target_property(_inc vibe86_${v} INTERFACE_INCLUDE_DIRECTORIES)
  set_property(TARGET vibe86_${v} APPEND PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${_inc})
  target_sources(vsim PRIVATE cpu_${v}.cpp)
  target_link_libraries(vsim PRIVATE vibe86_${v})
endforeach()
find_package(Threads REQUIRED)
target_link_libraries(vsim PRIVATE vsim_board PkgConfig::SDL3 Threads::Threads)
set_target_properties(vsim PROPERTIES BUILD_RPATH /usr/local/lib)

# tpmgen made tpm-debug.permall once (it is in the repository);
# tpm_debug_state.h embeds it. Rebuild both only to change the debug TPM's
# seeds: cmake --build build --target tpmgen
add_executable(tpmgen EXCLUDE_FROM_ALL tpmgen.c)
target_include_directories(tpmgen PRIVATE ${TPMS_INCLUDE_DIR})
target_link_libraries(tpmgen PRIVATE ${TPMS_LIBRARY})

# the pin-level DDR5 DIMM model's own test (no Verilated core needed)
add_executable(ddr5_test test/ddr5_test.c ddr5.c spd.c)
target_include_directories(ddr5_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(ddr5_test PRIVATE ${VSIM_WARN})
add_test(NAME sim/ddr5 COMMAND ddr5_test)
set_tests_properties(sim/ddr5 PROPERTIES LABELS sim)
