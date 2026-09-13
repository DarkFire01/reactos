
remove_definitions(-D_WIN32_WINNT=0x502)

include_directories(
    sys
    uacpi-host
    uacpi/include
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/arbiter)

# The driver provides the Vista ACPI contract
add_definitions(
    -D_WIN32_WINNT=0x600
    -DNTDDI_VERSION=0x06000000)

set(UACPI_SOURCES "")
macro(uacpi_add_sources)
    foreach(_src ${ARGN})
        list(APPEND UACPI_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/uacpi/source/${_src}")
    endforeach()
endmacro()
include(uacpi/source/files.cmake)

add_library(uacpi ${UACPI_SOURCES})
set_target_properties(uacpi PROPERTIES C_STANDARD 11)
target_compile_definitions(uacpi PUBLIC UACPI_OVERRIDE_LIBC)

# The arbiter layout depends on NTDDI_VERSION, so it is built with the driver's
set(ARBITER_SOURCE_DIR ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/arbiter)
add_library(acpi_arbiter
    ${ARBITER_SOURCE_DIR}/arbiter.c
    ${ARBITER_SOURCE_DIR}/entry.c
    ${ARBITER_SOURCE_DIR}/handler.c
    ${ARBITER_SOURCE_DIR}/ordering.c
    ${ARBITER_SOURCE_DIR}/range.c
    ${ARBITER_SOURCE_DIR}/transaction.c)
target_compile_definitions(acpi_arbiter PRIVATE _NTSYSTEM_)
add_dependencies(acpi_arbiter bugcodes xdk)

list(APPEND SOURCE
    sys/driver.c
    sys/enum.c
    sys/fdo.c
    sys/filter.c
    sys/glue.c
    sys/guid.c
    sys/hal.c
    sys/interfaces.c
    sys/ioctl.c
    sys/irqarb.c
    sys/irqlib.c
    sys/pdo.c
    sys/power.c
    sys/powerres.c
    sys/resarb.c
    sys/reshub.c
    sys/resource.c
    sys/sleep.c
    drvs/applaunch.c
    drvs/button.c
    drvs/devif.c
    drvs/ec.c
    drvs/fan.c
    drvs/processor.c
    drvs/thermal.c
    uacpi-host/uacpi_host.c
    uacpi-host/uacpi_isr.c)

add_library(acpi_new MODULE
    ${SOURCE}
    acpi.rc)

set_target_properties(acpi_new PROPERTIES C_STANDARD 11)
set_module_type(acpi_new kernelmodedriver)
target_link_libraries(acpi_new uacpi acpi_arbiter wdmguid memcmp ${PSEH_LIB})
add_importlibs(acpi_new wmilib ntoskrnl hal)
