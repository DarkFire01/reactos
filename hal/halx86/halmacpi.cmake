
list(APPEND HAL_HALMACPI_SOURCE
    #SMP
    generic/buildtype.c
    generic/spinlock.c
    #Generic
    generic/beep.c
    generic/cmos.c
    generic/display.c
    generic/dma.c
    generic/drive.c
    generic/halinit.c
    generic/memory.c
    generic/misc.c
    generic/nmi.c
    generic/pic.c
    generic/reboot.c
    generic/sysinfo.c
    generic/usage.c
    #ACPI
    acpi/halacpi.c
    acpi/halpnpdd.c
    acpi/busemul.c
    legacy/bus/pcibus.c
    #APIC
    apic/apic.c
    apic/apictimer.c
    apic/apicsmp.c
    apic/halinit.c
    apic/processor.c
    apic/rtctimer.c
    apic/tsc.c)

list(APPEND HAL_HALMACPI_ASM_SOURCE
    apic/apictrap.S
    apic/tsccal.S)

if(ARCH STREQUAL "i386")
    list(APPEND HAL_HALMACPI_SOURCE
        generic/bios.c
        generic/portio.c)

    list(APPEND HAL_HALMACPI_ASM_SOURCE
        generic/v86.S)
endif()

add_asm_files(lib_hal_halmacpi_asm ${HAL_HALMACPI_ASM_SOURCE})
add_library(lib_hal_halmacpi OBJECT ${HAL_HALMACPI_SOURCE} ${lib_hal_halmacpi_asm})
add_dependencies(lib_hal_halmacpi bugcodes xdk)
target_compile_definitions(lib_hal_halmacpi PRIVATE CONFIG_SMP)
