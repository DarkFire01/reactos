
list(APPEND HAL_APIC_ASM_SOURCE
    apic/apictrap.S
    apic/tsccal.S)

list(APPEND HAL_APIC_SOURCE
    apic/apic.c
    apic/apictimer.c
    apic/halinit.c
    apic/processor.c
    apic/rtctimer.c
    apic/tsc.c)

if(ARCH STREQUAL "amd64")
    add_definitions(-DWIN64)
endif()

add_asm_files(lib_hal_apic_asm ${HAL_APIC_ASM_SOURCE})
add_library(lib_hal_apic OBJECT ${HAL_APIC_SOURCE} ${lib_hal_apic_asm})
include_directories(${REACTOS_SOURCE_DIR}/drivers/bus/acpi/acpica/include)
add_dependencies(lib_hal_apic asm)
