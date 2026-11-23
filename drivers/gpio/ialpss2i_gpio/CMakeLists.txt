
list(APPEND SOURCE
    controller.c
    driver.c
    interrupt.c
    layout.c
    pins.c
    ialpss2i_gpio.h)

add_library(ialpss2i_gpio MODULE ${SOURCE} ialpss2i_gpio.rc)
set_module_type(ialpss2i_gpio kmdfdriver)

target_compile_definitions(ialpss2i_gpio PRIVATE
    WDF_DEVICE_NO_WDMSEC_H _WIN32_WINNT=0x603 NTDDI_VERSION=0x06030000)

target_link_libraries(ialpss2i_gpio msgpioclxstub ntoskrnl_vista libcntpr ${PSEH_LIB})
add_importlibs(ialpss2i_gpio wdfldr ntoskrnl hal)
add_cd_file(TARGET ialpss2i_gpio DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_driver_inf(ialpss2i_gpio ialpss2i_gpio.inf)
add_registry_inf(ialpss2i_gpio_reg.inf)
