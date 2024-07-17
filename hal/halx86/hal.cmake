include(up.cmake)

add_library(lib_hal_hal OBJECT
            ${HAL_UP_SOURCE}
            ${HAL_GENERIC_SOURCE}
            ${lib_hal_generic_asm}
            ${HAL_ACPI_SOURCE}
            ${HAL_APIC_SOURCE}
            ${lib_hal_apic_asm})
add_dependencies(lib_hal_hal bugcodes xdk asm)
target_compile_definitions(lib_hal_hal PRIVATE CONFIG_UP CONFIG_HAL)

