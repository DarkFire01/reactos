
remove_definitions(-D_WIN32_WINNT=0x502)

list(APPEND SOURCE
    descvalidation.cpp
    devconfig.cpp
    devdesc.cpp
    devlpm.cpp
    devmisc.cpp
    devucx.cpp
    devxfer.cpp
    driver.cpp
    hubutil.cpp
    wer.cpp
    dsm.cpp
    dsm_boot.cpp
    dsm_config.cpp
    dsm_desc.cpp
    dsm_enum.cpp
    dsm_hub.cpp
    dsm_lpm.cpp
    dsm_power.cpp
    dsm_reenum.cpp
    dsm_remove.cpp
    dsm_report.cpp
    dsm_stopped.cpp
    dsm_zero.cpp
    hsm.cpp
    guid.cpp
    hubdrom.cpp
    hubacpi.cpp
    hubboot.cpp
    hubchild.cpp
    hubconnector.cpp
    hubdescval.cpp
    hubfdo.cpp
    hubid.cpp
    hubidle.cpp
    hubioctl.cpp
    hubmux.cpp
    hubparent.cpp
    hubpdo.cpp
    hubmsos.cpp
    hubport.cpp
    hubregistry.cpp
    hubsha1.cpp
    hubtunnel.cpp
    hubucx.cpp
    hubwmi.cpp
    hubxfer.cpp
    portxfer.cpp
    ism.cpp
    psm.cpp
    psm_frames.cpp
    psm_port.cpp
    usbhub3.h)

spec2def(usbhub3.sys usbhub3.spec)

add_library(usbhub3 MODULE ${SOURCE} usbhub3.rc ${CMAKE_CURRENT_BINARY_DIR}/usbhub3.def)
set_module_type(usbhub3 kmdfdriver)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    # WDF_DECLARE_CONTEXT_TYPE passes a string literal as PCHAR
    target_compile_options(usbhub3 PRIVATE -Wno-write-strings)
endif()

target_compile_definitions(usbhub3 PRIVATE
    WDF_DEVICE_NO_WDMSEC_H _WIN32_WINNT=0x603 NTDDI_VERSION=0x06030000)

target_include_directories(usbhub3 PRIVATE ${REACTOS_SOURCE_DIR}/sdk/include/ucx/1.6)

target_link_libraries(usbhub3 usbdex ntoskrnl_vista libcntpr ${PSEH_LIB})
add_importlibs(usbhub3 wdfldr usbd ntoskrnl hal)
add_cd_file(TARGET usbhub3 DESTINATION reactos/system32/drivers NO_CAB FOR all)
