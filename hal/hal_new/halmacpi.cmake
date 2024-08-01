
add_library(lib_hal_halmacpi OBJECT
            ${HAL_GENERIC_SOURCE})

add_dependencies(lib_hal_halmacpi bugcodes xdk asm)
target_compile_definitions(lib_hal_halmacpi PRIVATE CONFIG_SMP CONFIG_HALMACPI)
