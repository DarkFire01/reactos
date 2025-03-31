
include_directories(
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/arbiter
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/drivers)

list(APPEND ACPI_SOURCE
    acpiinit.c
    amli.c
    dispatch.c
    registry.c
    acpi.h)

add_library(acpi MODULE ${ACPI_SOURCE} acpi.rc)
set_module_type(acpi kernelmodedriver)
#target_link_libraries(acpi wdmguid acpica memcmp strtol ${PSEH_LIB})
target_link_libraries(acpi arbiter memcmp)
add_importlibs(acpi ntoskrnl hal)
#add_pch(acpi acpi.h ACPI_SOURCE)
#add_pch(acpi precomp.h ACPI_SOURCE)
add_cd_file(TARGET acpi DESTINATION reactos/system32/drivers NO_CAB FOR all)
