remove_definitions(-D_WIN32_WINNT=0x502)

list(APPEND SOURCE
    acpi.c
    api.c
    driver.c
    fdo.c
    pdo.c
    rolemgr.c
    urscx.h)

add_library(urscx01000 MODULE ${SOURCE} urscx01000.rc)
set_module_type(urscx01000 kmdfdriver)

target_compile_definitions(urscx01000 PRIVATE
    WDF_DEVICE_NO_WDMSEC_H _WIN32_WINNT=0x603 NTDDI_VERSION=0x06030000)

target_include_directories(urscx01000 PRIVATE
    ${REACTOS_SOURCE_DIR}/sdk/include/urs/1.0
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/shared/inc/private/common
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/kmdf/inc/private)

target_link_libraries(urscx01000 ntoskrnl_vista libcntpr ${PSEH_LIB})
add_importlibs(urscx01000 wdfldr ntoskrnl hal)
add_cd_file(TARGET urscx01000 DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_registry_inf(urscx01000_reg.inf)
