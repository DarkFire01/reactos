
list(APPEND HAL_SMP_SOURCE
    #generic
    generic/spinlock.c
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
    legacy/bus/pcibus.c)

if(ARCH STREQUAL "i386")
    list(APPEND HAL_SMP_SOURCE
        generic/bios.c
        generic/portio.c)
    list(APPEND HAL_SMP_ASM_SOURCE
        generic/v86.S)
endif()

add_asm_files(lib_hal_smp_asm ${HAL_SMP_ASM_SOURCE})
add_library(lib_hal_smp OBJECT ${HAL_SMP_SOURCE} ${lib_hal_smp_asm})
target_compile_definitions(lib_hal_smp PRIVATE CONFIG_SMP)
add_dependencies(lib_hal_smp asm)
