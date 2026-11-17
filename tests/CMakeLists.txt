# tests/: programs that run on the latest core (VIBE86_LATEST) in vsim, from
# reset: each .S under tests/<group>/ is assembled with clang, linked into a
# flat flash image with ld.lld (flat.ld) and run by run.py, which checks the
# exit status and the serial output the source's comments ask for.

find_program(CLANG clang REQUIRED)
find_program(LLD ld.lld REQUIRED)

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*/*.S)
list(FILTER TEST_SOURCES EXCLUDE REGEX "/v?diff/")
foreach(src IN LISTS TEST_SOURCES)
  file(RELATIVE_PATH rel ${CMAKE_CURRENT_SOURCE_DIR} ${src})
  string(REGEX REPLACE "\\.S$" "" name ${rel})
  string(REPLACE "/" "-" tgt ${name})
  set(obj ${CMAKE_CURRENT_BINARY_DIR}/${name}.o)
  set(bin ${CMAKE_CURRENT_BINARY_DIR}/${name}.bin)
  get_filename_component(dir ${obj} DIRECTORY)
  add_custom_command(OUTPUT ${bin}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${dir}
    COMMAND ${CLANG} -c -target x86_64-unknown-none -I${CMAKE_CURRENT_SOURCE_DIR}/include
            -o ${obj} ${src}
    COMMAND ${LLD} -T ${CMAKE_CURRENT_SOURCE_DIR}/flat.ld -o ${bin} ${obj}
    DEPENDS ${src} ${CMAKE_CURRENT_SOURCE_DIR}/flat.ld
    COMMENT "test image ${name}"
    VERBATIM)
  add_custom_target(test-${tgt} ALL DEPENDS ${bin})
  add_test(NAME ${name}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run.py $<TARGET_FILE:vsim> ${bin} ${src})
endforeach()

# firmware: QEMU's SeaBIOS build (the "pc" machine's bios.bin) on the test board
set(VIBE86_SEABIOS /usr/local/share/qemu/bios.bin CACHE FILEPATH "SeaBIOS image (QEMU's bios.bin)")
if(EXISTS ${VIBE86_SEABIOS})
  add_test(NAME firmware/seabios
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run.py $<TARGET_FILE:vsim>
            ${VIBE86_SEABIOS} ${CMAKE_CURRENT_SOURCE_DIR}/firmware/seabios.test)
  set_tests_properties(firmware/seabios PROPERTIES LABELS slow TIMEOUT 21600
    ENVIRONMENT "ACPI=${VIBE86_ACPI_DIR};DATA=${CMAKE_CURRENT_SOURCE_DIR}/firmware")
else()
  message(STATUS "no SeaBIOS image (${VIBE86_SEABIOS}): firmware/seabios test skipped")
endif()

# firmware: the vibe86 UEFI firmware (fw/), tests/firmware/uefi-*.test;
# WORK is the git-ignored work/ directory with the user's binaries (requires:)
file(GLOB UEFI_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/firmware/uefi-*.test)
foreach(t IN LISTS UEFI_TESTS)
  get_filename_component(name ${t} NAME_WE)
  add_test(NAME firmware/${name}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run.py $<TARGET_FILE:vsim> ${VIBE86_FD} ${t})
  file(STRINGS ${t} labels REGEX "^// ctest-labels:")
  string(REGEX REPLACE "^// ctest-labels:[ ]*" "" labels "${labels}")
  string(REPLACE " " ";" labels "${labels}")
  set_tests_properties(firmware/${name} PROPERTIES LABELS "firmware;${labels}" TIMEOUT 3000 SKIP_RETURN_CODE 77
    ENVIRONMENT "ACPI=${VIBE86_ACPI_DIR};DATA=${CMAKE_CURRENT_SOURCE_DIR}/firmware;WORK=${CMAKE_SOURCE_DIR}/work")
endforeach()

add_subdirectory(diff)
add_subdirectory(vdiff)

# tools: unit tests of the SDM extractor and the microcode compiler (parser,
# inlining, forms and the generated decode tables of the latest version)
add_test(NAME tools/ucc
  COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_ucc.py)
set_tests_properties(tools/ucc PROPERTIES ENVIRONMENT
  "VIBE86_EXTRACTED=${UCC_SDM};VIBE86_UCODE_PKG=${CMAKE_BINARY_DIR}/rtl/${VIBE86_LATEST}/gen/${VIBE86_LATEST}_ucode_pkg.sv")

# jtag: the JTAG probe (vsim -D) end to end
add_subdirectory(jtag)
