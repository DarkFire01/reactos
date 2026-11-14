
list(APPEND SOURCE
    driver.c
    master.c
    osservice.c
    rpmb.c
    service.c
    wtrp.h)

add_library(windowstrustedrt MODULE ${SOURCE} windowstrustedrt.rc)
set_module_type(windowstrustedrt kmdfdriver)

target_compile_definitions(windowstrustedrt PRIVATE
    WDF_DEVICE_NO_WDMSEC_H _WIN32_WINNT=0x603 NTDDI_VERSION=0x06030000)

target_include_directories(windowstrustedrt PRIVATE
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/shared/inc/private/common
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/kmdf/inc/private)

target_link_libraries(windowstrustedrt ntoskrnl_vista libcntpr ${PSEH_LIB})
add_importlibs(windowstrustedrt wdfldr ntoskrnl hal)
add_cd_file(TARGET windowstrustedrt DESTINATION reactos/system32/drivers NO_CAB FOR all)
