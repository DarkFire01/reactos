
list(APPEND HAL_HALMP_SOURCE
    #SMP
    generic/buildtype.c
    generic/spinlock.c
    smp/smp.c
    #smp/mpmemory.c
    smp/ipi.c
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
    #Legacy
    legacy/bus/bushndlr.c
    legacy/bus/cmosbus.c
    legacy/bus/isabus.c
    legacy/bus/pcibus.c
    ${CMAKE_CURRENT_BINARY_DIR}/pci_classes.c
    ${CMAKE_CURRENT_BINARY_DIR}/pci_vendors.c
    legacy/bus/sysbus.c
    legacy/bussupp.c
    legacy/halpnpdd.c
    legacy/halpcat.c
    mp/mps.c
    #APIC
    apic/apic.c
    apic/apictimer.c
    apic/apicsmp.c
    apic/halinit.c
    apic/processor.c
    apic/rtctimer.c
    apic/tsc.c)

list(APPEND HAL_HALMP_ASM_SOURCE
    apic/apictrap.S
    apic/tsccal.S)

if(ARCH STREQUAL "i386")
list(APPEND HAL_HALMP_SOURCE
    generic/bios.c
    generic/portio.c)

list(APPEND HAL_HALMP_ASM_SOURCE
    generic/v86.S)
endif()

if(ARCH STREQUAL "i386")
list(APPEND HAL_HALMP_ASM_SOURCE
    smp/i386/apentry.S)
endif()

if(ARCH STREQUAL "amd64")
list(APPEND HAL_HALMP_ASM_SOURCE
    smp/amd64/apentry.S)
endif()

add_asm_files(lib_hal_halmp_asm ${HAL_HALMP_ASM_SOURCE})
add_library(lib_hal_halmp OBJECT ${HAL_HALMP_SOURCE} ${lib_hal_halmp_asm})
add_dependencies(lib_hal_halmp bugcodes xdk)
target_compile_definitions(lib_hal_halmp PRIVATE CONFIG_SMP)