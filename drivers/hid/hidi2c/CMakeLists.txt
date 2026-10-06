#
# HID over I2C. A KMDF driver that sits under the WDM hidclass through mshidkmdf,
# and reaches its peripheral through the resource hub and SpbCx.
#
#
#
remove_definitions(-D_WIN32_WINNT=0x502)

list(APPEND SOURCE
    driver.c
    hid.c
    transport.c
    hidi2cp.h)

add_library(hidi2c MODULE ${SOURCE} hidi2c.rc)
set_module_type(hidi2c kmdfdriver)
add_importlibs(hidi2c ntoskrnl hal)

target_include_directories(hidi2c PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/kmdf/inc/private
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/shared/inc/private/common)

add_cd_file(TARGET hidi2c DESTINATION reactos/system32/drivers NO_CAB FOR all)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(hidi2c PRIVATE -Wno-old-style-declaration)
endif()
