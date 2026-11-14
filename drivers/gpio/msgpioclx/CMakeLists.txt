
list(APPEND SOURCE
    banks.c
    client.c
    controller.c
    gpioclx.c
    interrupt.c
    pins.c
    reshub.c
    secint.c
    gpioclxp.h)

add_library(msgpioclx MODULE ${SOURCE} msgpioclx.rc)
set_module_type(msgpioclx kmdfdriver)

target_compile_definitions(msgpioclx PRIVATE _WIN32_WINNT=0x603 NTDDI_VERSION=0x06030000)

target_include_directories(msgpioclx PRIVATE
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/shared/inc/private/common
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/kmdf/inc/private)

target_link_libraries(msgpioclx ntoskrnl_vista libcntpr ${PSEH_LIB})
add_importlibs(msgpioclx wdfldr ntoskrnl hal)
add_cd_file(TARGET msgpioclx DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_registry_inf(msgpioclx_reg.inf)
