#
# Intel LPSS I2C controller: an SpbCx client driving a Synopsys DesignWare I2C
# core.
#
# The engine is not Intel-specific, since the same DesignWare core turns up
# behind several vendors' PnP ids, but the LPSS shell around it is, which is why
# lives under the Intel name the reference uses.
#
remove_definitions(-D_WIN32_WINNT=0x502)

list(APPEND SOURCE
    ../../inc/spbclient.c
    controller.c
    driver.c
    transfer.c
    i2cpriv.h)

add_library(ialpss2i_i2c MODULE ${SOURCE} ialpss2i_i2c.rc)
set_module_type(ialpss2i_i2c kmdfdriver)
add_importlibs(ialpss2i_i2c ntoskrnl hal)

target_include_directories(ialpss2i_i2c PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../inc
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/kmdf/inc/private
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/shared/inc/private/common)

add_cd_file(TARGET ialpss2i_i2c DESTINATION reactos/system32/drivers NO_CAB FOR all)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(ialpss2i_i2c PRIVATE -Wno-old-style-declaration)
endif()
