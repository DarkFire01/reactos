
list(APPEND SOURCE
    acpi.c
    driver.c
    hid.c
    spb.c
    hidi2c.h)

add_library(hidi2c MODULE ${SOURCE} hidi2c.rc)
set_module_type(hidi2c kmdfdriver)

target_compile_definitions(hidi2c PRIVATE
    WDF_DEVICE_NO_WDMSEC_H _WIN32_WINNT=0x603 NTDDI_VERSION=0x06030000)

target_link_libraries(hidi2c ntoskrnl_vista libcntpr ${PSEH_LIB})
add_importlibs(hidi2c wdfldr ntoskrnl hal)
add_cd_file(TARGET hidi2c DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_driver_inf(hidi2c hidi2c.inf)
add_registry_inf(hidi2c_reg.inf)
