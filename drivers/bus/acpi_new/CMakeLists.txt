set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

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
    source/event.c
    source/mutex.c
    source/osi.c)

#add_pch(uacpi include/uacpi/acpi.h SOURCE) idk

list(APPEND ACPI_SOURCE
    main.c
    uacpi_layer.c)

include_directories(
    "include/uacpi/")
    include_directories(
        "include/uacpi/platform")
        include_directories(
            "include/uacpi/internal")
add_library(acpi MODULE
    ${UACPI_SOURCE}
    ${ACPI_SOURCE}
    acpi.rc)

set_module_type(acpi kernelmodedriver)
add_importlibs(acpi ntoskrnl hal)
#add_pch(acpi precomp.h ACPI_SOURCE)
add_cd_file(TARGET acpi DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_driver_inf(acpi acpi.inf)
