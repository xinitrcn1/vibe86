# tests/diff: differential testing of the SSE and SSE2 instruction sets against
# the host CPU (W. M. McKeeman, "Differential Testing for Software", Digital
# Technical Journal 10(1), 1998). vgen.py writes a random body per seed; it
# runs natively (host.c) and on vsim (sim.S) and vdiff.py compares the
# per-instruction hashes. Needs an x86-64 host.

if(NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "amd64|AMD64|x86_64")
  message(STATUS "tests/diff: not an x86-64 host, differential tests skipped")
  return()
endif()

set(VIBE86_VDIFF_SEEDS 1 2 3 4 CACHE STRING "differential test seeds")
set(VIBE86_VDIFF_STEPS 400 CACHE STRING "instructions per differential test")
set(d ${CMAKE_CURRENT_SOURCE_DIR})
foreach(seed IN LISTS VIBE86_VDIFF_SEEDS)
  set(o ${CMAKE_CURRENT_BINARY_DIR}/seed${seed})
  add_custom_command(OUTPUT ${o}/body.S ${o}/body.lst
    COMMAND ${CMAKE_COMMAND} -E make_directory ${o}
    COMMAND ${Python3_EXECUTABLE} ${d}/vgen.py ${seed} ${VIBE86_VDIFF_STEPS} ${o}/body.S ${o}/body.lst
    DEPENDS ${d}/vgen.py
    VERBATIM)
  add_custom_command(OUTPUT ${o}/host
    COMMAND ${CLANG} -O1 -o ${o}/host ${d}/host.c ${o}/body.S
    DEPENDS ${d}/host.c ${o}/body.S
    VERBATIM)
  add_custom_command(OUTPUT ${o}/sim.bin
    COMMAND ${CLANG} -c -target x86_64-unknown-none -I${d}/../include
            "-DDIFF_BODY=\"${o}/body.S\"" -o ${o}/sim.o ${d}/sim.S
    COMMAND ${LLD} -T ${d}/../flat.ld -o ${o}/sim.bin ${o}/sim.o
    DEPENDS ${d}/sim.S ${o}/body.S ${d}/../include/boot64.inc
    VERBATIM)
  add_custom_target(vdiff-seed${seed} ALL DEPENDS ${o}/host ${o}/sim.bin)
  add_test(NAME vdiff/seed${seed}
    COMMAND ${Python3_EXECUTABLE} ${d}/vdiff.py ${o}/host $<TARGET_FILE:vsim> ${o}/sim.bin ${o}/body.lst)
endforeach()
