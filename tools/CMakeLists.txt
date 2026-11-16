# tools/ucc: the SDM-to-microcode compiler. vibe86_ucode(<version>) generates
# a core version's micro-op ISA package, microcode ROM and decoder tables
# (tools/ucc/targets/<version>/) into the build tree.

set(UCC ${CMAKE_CURRENT_SOURCE_DIR}/ucc/ucc.py CACHE INTERNAL "")
set(UCC_SDM ${CMAKE_CURRENT_SOURCE_DIR}/sdm/extracted CACHE INTERNAL "")
file(GLOB UCC_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ucc/*.py)
set(UCC_SOURCES ${UCC_SOURCES} CACHE INTERNAL "")
file(GLOB UCC_SDM_PAGES CONFIGURE_DEPENDS ${UCC_SDM}/*.sdm)
set(UCC_SDM_PAGES ${UCC_SDM_PAGES} CACHE INTERNAL "")

function(vibe86_ucode version)
  set(tdir ${PROJECT_SOURCE_DIR}/tools/ucc/targets/${version})
  set(gen ${CMAKE_CURRENT_BINARY_DIR}/gen)
  file(GLOB_RECURSE tfiles CONFIGURE_DEPENDS ${tdir}/*)
  # verilate() reads its sources when CMake configures: generate them now
  # once, and again at build time whenever an input changes
  if(NOT EXISTS ${gen}/${version}_ucode_pkg.sv)
    file(MAKE_DIRECTORY ${gen})
    execute_process(
      COMMAND ${Python3_EXECUTABLE} -B ${UCC} --target ${version} --sdm ${UCC_SDM}
              --pkg ${gen}/${version}_uop_pkg.sv --out ${gen}/${version}_ucode_pkg.sv
              --listing ${gen}/${version}.lst --report ${gen}/${version}-report.md
      RESULT_VARIABLE rc)
    if(rc)
      message(FATAL_ERROR "ucc failed for ${version}")
    endif()
  endif()
  add_custom_command(
    OUTPUT ${gen}/${version}_uop_pkg.sv ${gen}/${version}_ucode_pkg.sv
           ${gen}/${version}.lst ${gen}/${version}-report.md
    COMMAND ${CMAKE_COMMAND} -E make_directory ${gen}
    COMMAND ${Python3_EXECUTABLE} -B ${UCC} --target ${version} --sdm ${UCC_SDM}
            --pkg ${gen}/${version}_uop_pkg.sv --out ${gen}/${version}_ucode_pkg.sv
            --listing ${gen}/${version}.lst --report ${gen}/${version}-report.md
    DEPENDS ${UCC_SOURCES} ${tfiles} ${UCC_SDM_PAGES}
    COMMENT "ucc: microcode for ${version} from the Intel SDM"
    VERBATIM)
  add_custom_target(ucode_${version} DEPENDS ${gen}/${version}_ucode_pkg.sv)
  set(${version}_UOP_PKG ${gen}/${version}_uop_pkg.sv PARENT_SCOPE)
  set(${version}_UCODE_PKG ${gen}/${version}_ucode_pkg.sv PARENT_SCOPE)
endfunction()
