
add_library(windowstrustedrtproxy MODULE proxy.c windowstrustedrtproxy.rc)
set_module_type(windowstrustedrtproxy kmdfdriver)

target_compile_definitions(windowstrustedrtproxy PRIVATE
    WDF_DEVICE_NO_WDMSEC_H _WIN32_WINNT=0x603 NTDDI_VERSION=0x06030000)

target_include_directories(windowstrustedrtproxy PRIVATE
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/kmdf/inc/private)

target_link_libraries(windowstrustedrtproxy ntoskrnl_vista libcntpr)
add_importlibs(windowstrustedrtproxy wdfldr ntoskrnl hal)
add_cd_file(TARGET windowstrustedrtproxy DESTINATION reactos/system32/drivers NO_CAB FOR all)
