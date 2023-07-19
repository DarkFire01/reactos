
list(APPEND HAL_APICSMP_ASM_SOURCE
    apic/apictrap.S
    apic/tsccal.S)

list(APPEND HAL_APICSMP_SOURCE
    apic/apic.c
    apic/apictimer.c
    apic/apicsmp.c
    apic/halinit.c
    apic/processor.c
    apic/rtctimer.c
    apic/tsc.c)

add_asm_files(lib_hal_apicsmp_asm ${HAL_APICSMP_ASM_SOURCE})
add_library(lib_hal_apicsmp OBJECT ${HAL_APIC_SOURCE} ${lib_hal_apicsmp_asm})
add_dependencies(lib_hal_apicsmp asm)
