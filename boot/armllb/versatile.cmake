
list(APPEND LIB_VERSATILE_SOURCE
    hw/versatile/hwinfo.c
    hw/versatile/hwinit.c
    hw/versatile/hwclcd.c
    hw/versatile/hwuart.c
    hw/versatile/hwkmi.c)

add_library(lib_board_versatile OBJECT ${LIB_VERSATILE_SOURCE})
add_dependencies(lib_board_versatile xdk)
