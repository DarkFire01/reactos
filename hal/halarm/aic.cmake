

list(APPEND HAL_AIC_SOURCE
    apple/aic.c
    apple/halinit_up.c)

# Needed to compile while using ACPICA
if(ARCH STREQUAL "arm64")
    add_definitions(-DWIN64)
endif()

add_library(lib_hal_aic OBJECT ${HAL_AIC_SOURCE})
add_pch(lib_hal_aic include/hal.h ${HAL_AIC_SOURCE})
add_dependencies(lib_hal_aic bugcodes xdk)
