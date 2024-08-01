
add_library(lib_hal_halaacpi OBJECT
            ${HAL_GENERIC_SOURCE}
            ${HAL_UP_SOURCE}
            ${HAL_EFI_SOURCE}
            ${HAL_X86_SOURCE}
            ${HAL_ACPI_SOURCE}
            ${HAL_APIC_SOURCE})

add_dependencies(lib_hal_halaacpi bugcodes xdk asm)
target_compile_definitions(lib_hal_halaacpi PRIVATE CONFIG_UP CONFIG_HALAACPI)
