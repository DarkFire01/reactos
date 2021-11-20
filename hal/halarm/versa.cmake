
list(APPEND HAL_VERSA_SOURCE
    versa/halinit_up.c
    versa/halup.rc)
    
add_library(lib_hal_versa OBJECT ${HAL_VERSA_SOURCE})
