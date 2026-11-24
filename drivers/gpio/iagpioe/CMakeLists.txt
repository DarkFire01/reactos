list(APPEND SOURCE
    controller.c
    driver.c
    pads.c
    pins.c
    iagpioe.h)

add_library(iagpioe MODULE ${SOURCE} iagpioe.rc)
set_module_type(iagpioe kmdfdriver)

target_compile_definitions(iagpioe PRIVATE
    WDF_DEVICE_NO_WDMSEC_H _WIN32_WINNT=0x603 NTDDI_VERSION=0x06030000)

target_link_libraries(iagpioe msgpioclxstub ntoskrnl_vista libcntpr ${PSEH_LIB})
add_importlibs(iagpioe wdfldr acpi ntoskrnl hal)
add_cd_file(TARGET iagpioe DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_driver_inf(iagpioe iagpioe.inf)
add_registry_inf(iagpioe_reg.inf)
