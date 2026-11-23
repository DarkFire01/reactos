
list(APPEND SOURCE
    controller.c
    driver.c
    platform.c
    transfer.c
    ialpss2i_i2c.h)

add_library(ialpss2i_i2c MODULE ${SOURCE} ialpss2i_i2c.rc)
set_module_type(ialpss2i_i2c kmdfdriver)

target_compile_definitions(ialpss2i_i2c PRIVATE
    WDF_DEVICE_NO_WDMSEC_H _WIN32_WINNT=0x603 NTDDI_VERSION=0x06030000)

target_link_libraries(ialpss2i_i2c spbcxstub ntoskrnl_vista libcntpr ${PSEH_LIB})
add_importlibs(ialpss2i_i2c wdfldr ntoskrnl hal)
add_cd_file(TARGET ialpss2i_i2c DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_driver_inf(ialpss2i_i2c ialpss2i_i2c.inf)
add_registry_inf(ialpss2i_i2c_reg.inf)
