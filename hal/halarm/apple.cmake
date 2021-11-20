
list(APPEND HAL_APPLE_SOURCE
    apple/halinit.c
    apple/halup.rc)

add_library(lib_hal_apple OBJECT ${HAL_APPLE_SOURCE})
