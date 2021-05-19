
list(APPEND HAL_HALMACPI_SOURCE
    apic/apic.c
    apic/apictimer.c
    apic/halinit.c
    apic/processor.c
    apic/rtctimer.c
    apic/tsc.c)

list(APPEND HAL_HALMACPI_ASM_SOURCE
    apic/apictrap.S
    apic/tsccal.S)

add_asm_files(lib_hal_halmacpi_asm ${HAL_HALMACPI_ASM_SOURCE})
add_library(lib_hal_halmacpi OBJECT ${HAL_HALMACPI_SOURCE} ${lib_hal_halmacpi_asm})
target_compile_definitions(lib_hal_halmacpi PRIVATE CONFIG_SMP)
add_dependencies(lib_hal_halmacpi asm)
