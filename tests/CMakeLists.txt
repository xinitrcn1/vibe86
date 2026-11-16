# tests/: programs that run on the latest core (VIBE86_LATEST) in vsim, from
# reset: each .S under tests/<group>/ is assembled with clang, linked into a
# flat flash image with ld.lld (flat.ld) and run by run.py, which checks the
# exit status and the serial output the source's comments ask for.

find_program(CLANG clang REQUIRED)
find_program(LLD ld.lld REQUIRED)

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*/*.S)
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
