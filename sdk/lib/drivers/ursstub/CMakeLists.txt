remove_definitions(-D_WIN32_WINNT=0x502)

add_library(ursstub STATIC ursstub.c ursstub.h)
add_dependencies(ursstub xdk)

target_compile_definitions(ursstub
    PUBLIC WDF_DEVICE_NO_WDMSEC_H _WIN32_WINNT=0x603 NTDDI_VERSION=0x06030000)

target_include_directories(ursstub
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    ${REACTOS_SOURCE_DIR}/sdk/include/urs/1.0
    ${REACTOS_SOURCE_DIR}/sdk/include/wdf/kmdf/1.33
    PRIVATE ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/shared/inc/private/common
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/kmdf/inc/private)
