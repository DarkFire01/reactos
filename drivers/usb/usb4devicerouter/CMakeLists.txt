remove_definitions(-D_WIN32_WINNT=0x502)

list(APPEND SOURCE
    adapter.cpp
    driver.cpp
    drom.cpp
    eventpump.cpp
    fdo.cpp
    forward.cpp
    guid.cpp
    hostlink.cpp
    pdo.cpp
    port.cpp
    protoadapter.cpp
    router.cpp
    routerop.cpp
    segments.cpp
    usb4dr.h)

add_library(usb4devicerouter MODULE ${SOURCE} usb4devicerouter.rc)
set_module_type(usb4devicerouter kmdfdriver)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    # WDF_DECLARE_CONTEXT_TYPE passes a string literal as PCHAR
    target_compile_options(usb4devicerouter PRIVATE -Wno-write-strings)
endif()

target_compile_definitions(usb4devicerouter PRIVATE
    WDF_DEVICE_NO_WDMSEC_H _WIN32_WINNT=0x603 NTDDI_VERSION=0x06030000)

target_link_libraries(usb4devicerouter ntoskrnl_vista libcntpr ${PSEH_LIB})
add_importlibs(usb4devicerouter wdfldr ntoskrnl hal)
add_cd_file(TARGET usb4devicerouter DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_driver_inf(usb4devicerouter usb4devicerouter.inf)
