
list(APPEND LIB_GENERIC_SOURCE
    main.c
    crtsupp.c
    envir.c
    fw.c
    os/loader.c
    hw/keyboard.c
    hw/matrix.c
    hw/serial.c
    hw/video.c
    hw/time.c)

add_library(lib_board_generic OBJECT ${LIB_GENERIC_SOURCE})
add_dependencies(lib_board_generic xdk asm)
