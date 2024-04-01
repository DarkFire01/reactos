
list(APPEND HAL_CORE_SOURCE
     ${CMAKE_SOURCE_DIR}/hal_new/core/halinit.c
     ${CMAKE_SOURCE_DIR}/hal_new/core/kdmisc.c
     ${CMAKE_SOURCE_DIR}/hal_new/core/misc.c)

add_library(lib_hal_core OBJECT ${HAL_CORE_SOURCE})

