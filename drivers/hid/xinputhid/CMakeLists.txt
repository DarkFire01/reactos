# XInput HID Filter Driver - KMDF filter for HID game controllers

remove_definitions(-D_WIN32_WINNT=0x502)

list(APPEND SOURCE
    driver.c
    xinputhid.rc
)

add_library(xinputhid MODULE ${SOURCE})
set_module_type(xinputhid kmdfdriver)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(xinputhid PRIVATE -Wno-incompatible-pointer-types)
endif()

target_compile_definitions(xinputhid PRIVATE
    WDF_DEVICE_NO_WDMSEC_H
    _WIN32_WINNT=0x603
    NTDDI_VERSION=0x06030000
)

target_link_libraries(xinputhid ntoskrnl_vista hidparser_km ${PSEH_LIB})
add_importlibs(xinputhid hidparse wdfldr ntoskrnl hal)
add_cd_file(TARGET xinputhid DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_registry_inf(xinputhid_reg.inf)
add_driver_inf(xinputhid xinputhid.inf)
