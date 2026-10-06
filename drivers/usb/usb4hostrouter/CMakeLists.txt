remove_definitions(-D_WIN32_WINNT=0x502)

list(APPEND SOURCE
    configaccess.cpp
    driver.cpp
    guid.cpp
    hardware.cpp
    hostrouter.cpp
    interrupts.cpp
    powerpdo.cpp
    ring.cpp
    rootrouter.cpp
    topology.cpp
    tunnel.cpp
    usb4hr.h)

add_library(usb4hostrouter MODULE ${SOURCE} usb4hostrouter.rc)
set_module_type(usb4hostrouter kmdfdriver)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    # WDF_DECLARE_CONTEXT_TYPE passes a string literal as PCHAR
    target_compile_options(usb4hostrouter PRIVATE -Wno-write-strings)
endif()

target_compile_definitions(usb4hostrouter PRIVATE
    WDF_DEVICE_NO_WDMSEC_H _WIN32_WINNT=0x603 NTDDI_VERSION=0x06030000)

target_link_libraries(usb4hostrouter ntoskrnl_vista libcntpr ${PSEH_LIB})
add_importlibs(usb4hostrouter wdfldr ntoskrnl hal)
add_cd_file(TARGET usb4hostrouter DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_driver_inf(usb4hostrouter usb4hostrouter.inf)
