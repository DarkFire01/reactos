#
# RtEthSample, Microsoft's RTL8168 NetAdapterCx client sample (MIT, see
# LICENSE). It is the reference consumer for the class extension in
# drivers/network/netcx and shares that directory's inc/ shims, so it does not
# compile yet either.
#
# Upstream targets KMDF 1.23 and NetAdapterCx 2.0. The tree currently carries
# KMDF 1.33.
#
remove_definitions(-D_WIN32_WINNT=0x502)

list(APPEND SOURCE
    adapter.cpp
    configuration.cpp
    device.cpp
    driver.cpp
    eeprom.cpp
    gigamac.cpp
    interrupt.cpp
    link.cpp
    phy.cpp
    power.cpp
    rxqueue.cpp
    txqueue.cpp
    precomp.h)

add_library(rtethsample MODULE ${SOURCE} rtethsample.rc)
add_pch(rtethsample precomp.h SOURCE)
set_module_type(rtethsample kmdfdriver)
add_importlibs(rtethsample ndis ntoskrnl hal)

target_include_directories(rtethsample PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${REACTOS_SOURCE_DIR}/sdk/include/ddk/netcx
    ${REACTOS_SOURCE_DIR}/drivers/network/netcx/inc
    ${REACTOS_SOURCE_DIR}/drivers/usb/usb3-reactos/inc
    ${REACTOS_SOURCE_DIR}/drivers/usb/usb3-reactos/inc/pci)

target_compile_definitions(rtethsample PRIVATE
    _KERNEL_MODE=1
    NDIS_MINIPORT_DRIVER=1
    NDIS680_MINIPORT=1)

add_cd_file(TARGET rtethsample DESTINATION reactos/system32/drivers FOR all)
add_driver_inf(rtethsample netrteth.inf)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(rtethsample
        PRIVATE -Wno-unknown-pragmas -Wno-unused-parameter
        PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-reorder -Wno-invalid-offsetof>)
endif()
