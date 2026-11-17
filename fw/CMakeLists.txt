# fw/: platform firmware data. acpi/: the ACPI tables (the DSDT in ASL,
# compiled by iasl; the rest built by mktables.py) and QEMU's table-loader
# script, offered to firmware through fw_cfg (vsim -f etc/acpi/...).

find_program(IASL iasl REQUIRED)

set(ACPI_OUT ${CMAKE_CURRENT_BINARY_DIR}/acpi)
add_custom_command(
  OUTPUT ${ACPI_OUT}/dsdt.aml ${ACPI_OUT}/tables ${ACPI_OUT}/rsdp ${ACPI_OUT}/table-loader
  COMMAND ${CMAKE_COMMAND} -E make_directory ${ACPI_OUT}
  COMMAND ${IASL} -vs -p ${ACPI_OUT}/dsdt ${CMAKE_CURRENT_SOURCE_DIR}/acpi/dsdt.asl
  COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/acpi/mktables.py
          ${ACPI_OUT}/dsdt.aml ${ACPI_OUT}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/acpi/dsdt.asl ${CMAKE_CURRENT_SOURCE_DIR}/acpi/mktables.py
  COMMENT "ACPI tables (iasl, mktables.py)"
  VERBATIM)
add_custom_target(acpi ALL DEPENDS ${ACPI_OUT}/tables)
set(VIBE86_ACPI_DIR ${ACPI_OUT} CACHE INTERNAL "")
