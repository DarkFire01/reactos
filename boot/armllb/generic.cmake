
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

list(APPEND LIB_VERSATILE_ASM_SOURCE
    hw/versatile/boot.S)

add_asm_files(lib_generic_asm ${LIB_VERSATILE_ASM_SOURCE})
add_library(lib_board_generic OBJECT ${LIB_GENERIC_SOURCE} ${lib_generic_asm})
add_dependencies(lib_board_generic xdk asm)
