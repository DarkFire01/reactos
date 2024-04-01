
list(APPEND HAL_SMP_SOURCE
     ${CMAKE_SOURCE_DIR}/hal_new/smp/irql.c
     ${CMAKE_SOURCE_DIR}/hal_new/smp/manage.c)

add_library(lib_hal_smp OBJECT ${HAL_SMP_SOURCE})
target_compile_definitions(lib_hal_smp PRIVATE CONFIG_SMP)