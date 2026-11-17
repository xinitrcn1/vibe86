# tools/ucc: the SDM-to-microcode compiler. vibe86_ucode(<version>) generates
# a core version's micro-op ISA package, microcode ROM and decoder tables
# (tools/ucc/targets/<version>/) into the build tree.

set(UCC ${CMAKE_CURRENT_SOURCE_DIR}/ucc/ucc.py CACHE INTERNAL "")
file(GLOB UCC_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ucc/*.py)
set(UCC_SOURCES ${UCC_SOURCES} CACHE INTERNAL "")

# The Intel SDM, Vol. 2 (2A-2D), revision 325383-093US: the specification the
# microcode is compiled from (D-002). Pass its path with -DVIBE86_SDM_PDF=, or
# it is downloaded once into the build tree; either way its SHA-256 must match,
# so every build compiles the same text. extract.py turns it into the
# per-instruction .sdm pages ucc reads (build/tools/sdm/extracted); nothing
# generated is kept in the source tree.
set(VIBE86_SDM_PDF "" CACHE FILEPATH "Intel SDM Vol. 2 PDF, order no. 325383-093US (downloaded if empty)")
set(SDM_URL "https://cdrdv2.intel.com/v1/dl/getContent/671110")
set(SDM_SHA256 d137a7887787bbb06c1a8b8bafd51958d8d2e16b4dff987a03226c845cca1516)
set(SDM_PDF ${VIBE86_SDM_PDF})
if(NOT SDM_PDF)
  set(SDM_PDF ${CMAKE_BINARY_DIR}/sdm/325383-093US.pdf)
  if(NOT EXISTS ${SDM_PDF})
    message(STATUS "Downloading the Intel SDM Vol. 2 (325383-093US)")
    file(DOWNLOAD ${SDM_URL} ${SDM_PDF} STATUS st TLS_VERIFY ON)
    list(GET st 0 rc)
    if(rc)
      file(REMOVE ${SDM_PDF})
      message(FATAL_ERROR "SDM download failed (${st}); pass -DVIBE86_SDM_PDF=<325383-093US.pdf>")
    endif()
  endif()
endif()
file(SHA256 ${SDM_PDF} got)
if(NOT got STREQUAL SDM_SHA256)
  message(FATAL_ERROR "${SDM_PDF} isn't SDM Vol. 2 revision 325383-093US (SHA-256 ${got}); "
                      "Intel's download link serves the latest revision: pass the 093US PDF "
                      "with -DVIBE86_SDM_PDF=")
endif()

set(UCC_SDM ${CMAKE_CURRENT_BINARY_DIR}/sdm/extracted CACHE INTERNAL "")
set(SDM_STAMP ${CMAKE_CURRENT_BINARY_DIR}/sdm/extracted.stamp)
set(EXTRACT ${CMAKE_CURRENT_SOURCE_DIR}/sdm/extract.py)
# at configure time (verilate() needs the generated RTL then), and again
# whenever extract.py changes
if(NOT EXISTS ${SDM_STAMP} OR ${EXTRACT} IS_NEWER_THAN ${SDM_STAMP})
  message(STATUS "Extracting the SDM's instruction pages")
  file(REMOVE_RECURSE ${UCC_SDM})
  execute_process(COMMAND ${Python3_EXECUTABLE} -B ${EXTRACT} ${SDM_PDF} ${UCC_SDM}
                  RESULT_VARIABLE rc)
  if(rc)
    message(FATAL_ERROR "tools/sdm/extract.py failed")
  endif()
  file(TOUCH ${SDM_STAMP})
endif()
add_custom_command(OUTPUT ${SDM_STAMP}
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${UCC_SDM}
  COMMAND ${Python3_EXECUTABLE} -B ${EXTRACT} ${SDM_PDF} ${UCC_SDM}
  COMMAND ${CMAKE_COMMAND} -E touch ${SDM_STAMP}
  DEPENDS ${EXTRACT} ${SDM_PDF}
  COMMENT "extract.py: the SDM's instruction pages"
  VERBATIM)
add_custom_target(sdm_pages DEPENDS ${SDM_STAMP})
set(UCC_SDM_PAGES ${SDM_STAMP} CACHE INTERNAL "")

function(vibe86_ucode version)
  set(tdir ${PROJECT_SOURCE_DIR}/tools/ucc/targets/${version})
  set(gen ${CMAKE_CURRENT_BINARY_DIR}/gen)
  file(GLOB_RECURSE tfiles CONFIGURE_DEPENDS ${tdir}/*)
  # verilate() reads its sources when CMake configures: generate them now
  # once, and again at build time whenever an input changes
  # (also when an input is newer than the outputs: configuring verilates the
  # generated packages, so stale ones fail there before the build can redo them)
  file(GLOB ucc_src ${PROJECT_SOURCE_DIR}/tools/ucc/*.py)
  set(stale FALSE)
  foreach(f IN LISTS tfiles ucc_src)
    if(f IS_NEWER_THAN ${gen}/${version}_uop_pkg.sv)
      set(stale TRUE)
    endif()
  endforeach()
  if(NOT EXISTS ${gen}/${version}_ucode_pkg.sv OR stale)
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
  add_dependencies(ucode_${version} sdm_pages)
  set(${version}_UOP_PKG ${gen}/${version}_uop_pkg.sv PARENT_SCOPE)
  set(${version}_UCODE_PKG ${gen}/${version}_ucode_pkg.sv PARENT_SCOPE)
endfunction()
