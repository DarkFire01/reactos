
if(NOT ARCH STREQUAL "arm")
add_subdirectory(acpi_old)
endif()
add_subdirectory(isapnp)
#add_subdirectory(pci)
add_subdirectory(pcix)
add_subdirectory(pcmcia)
