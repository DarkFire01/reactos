
list(APPEND HAL_I386_SOURCE
     ${CMAKE_SOURCE_DIR}/hal_new/architecture/halx86/i386/portio.c
     ${CMAKE_SOURCE_DIR}/hal_new/architecture/halx86/x86bios.c
     ${CMAKE_SOURCE_DIR}/hal_new/architecture/halx86/reboot.c)

add_library(lib_hal_i386 OBJECT ${HAL_I386_SOURCE})

