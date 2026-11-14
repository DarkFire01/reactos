
list(APPEND SOURCE
    capture.c
    controller.c
    request.c
    spbcx.c
    target.c
    spbcxp.h)

add_library(spbcx MODULE ${SOURCE} spbcx.rc)
set_module_type(spbcx kmdfdriver)

target_compile_definitions(spbcx PRIVATE
    WDF_DEVICE_NO_WDMSEC_H _WIN32_WINNT=0x603 NTDDI_VERSION=0x06030000)

target_include_directories(spbcx PRIVATE
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/shared/inc/private/common
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/kmdf/inc/private)

target_link_libraries(spbcx ntoskrnl_vista libcntpr ${PSEH_LIB})
add_importlibs(spbcx wdfldr ntoskrnl hal)
add_cd_file(TARGET spbcx DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_registry_inf(spbcx_reg.inf)
