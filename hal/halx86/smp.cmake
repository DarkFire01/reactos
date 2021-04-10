list(APPEND HAL_SMP_SOURCE
    generic/spinlock.c
    #smp/smp.c
    )

list(APPEND HAL_SMP_ASM_SOURCE
    #smp/apboot.S
    )

add_asm_files(lib_hal_smp_asm ${HAL_SMP_ASM_SOURCE})
add_library(lib_hal_smp OBJECT ${HAL_SMP_SOURCE} ${lib_hal_smp_asm})
add_dependencies(lib_hal_smp asm bugcodes xdk)
