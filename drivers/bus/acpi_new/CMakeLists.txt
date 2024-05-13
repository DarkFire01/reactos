
include_directories(
    include)

list(APPEND UACPI_SOURCE
    source/tables.c
    source/types.c
    source/uacpi.c
    source/utilities.c
    source/interpreter.c
    source/opcodes.c
    source/namespace.c
    source/stdlib.c
    source/shareable.c
    source/opregion.c
    source/default_handlers.c
    source/io.c
    source/notify.c
    source/sleep.c
    source/registers.c
    source/resources.c
    source/event.c)

add_library(uacpi ${UACPI_SOURCE})
#add_pch(uacpi include/uacpi/acpi.h SOURCE) idk

list(APPEND ACPI_SOURCE
    main.c)

add_library(acpi_new MODULE
    ${ACPI_SOURCE}
    acpi.rc)

set_module_type(acpi_new kernelmodedriver)
target_link_libraries(acpi_new uacpi)
add_importlibs(acpi_new ntoskrnl hal)
#add_pch(acpi_new precomp.h ACPI_SOURCE)
add_cd_file(TARGET acpi_new DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_driver_inf(acpi_new acpi.inf)
