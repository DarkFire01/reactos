#
# Intel LPSS GPIO controller: a GpioClx client driving the pad hardware behind
# Intel's PCH GPIO communities.
#
# All four SoC variants Intel ships are the same engine, so this follows the
# I2C side: one driver, with the per-SoC community table as data.
#
remove_definitions(-D_WIN32_WINNT=0x502)

list(APPEND SOURCE
    ../inc/gpioclient.c
    controller.c
    interrupt.c
    pins.c
    driver.c
    gpiopriv.h)

add_library(ialpss2i_gpio MODULE ${SOURCE} ialpss2i_gpio.rc)
set_module_type(ialpss2i_gpio kmdfdriver)
add_importlibs(ialpss2i_gpio ntoskrnl hal)

target_include_directories(ialpss2i_gpio PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../inc
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/kmdf/inc/private
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/shared/inc/private/common)

add_cd_file(TARGET ialpss2i_gpio DESTINATION reactos/system32/drivers NO_CAB FOR all)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(ialpss2i_gpio PRIVATE -Wno-old-style-declaration)
endif()
