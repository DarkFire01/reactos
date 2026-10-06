#
# GpioClx: the KMDF class extension a GPIO controller driver binds to.
# The controller driver owns the silicon; this owns the pins a peripheral
# opens, interrupt demultiplexing, and the connection ids the resource hub
# mints against them.
#
# A KMDF client like spbcx and ucx01000, so
# the same floor dance: linking wdfdriverentry publishes the KMDF 1.17 headers
# and the NTDDI floor they need, which collides with the tree default unless
# that comes off first.
#
remove_definitions(-D_WIN32_WINNT=0x502)

list(APPEND SOURCE
    banks.c
    client.c
    controller.c
    interrupt.c
    pins.c
    reshub.c
    secint.c
    gpioclx.c
    gpioclxp.h)

add_library(gpioclx MODULE ${SOURCE} gpioclx.rc)
set_module_type(gpioclx kmdfdriver)
add_importlibs(gpioclx ntoskrnl hal)

target_include_directories(gpioclx PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/kmdf/inc/private
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/shared/inc/private/common)

add_cd_file(TARGET gpioclx DESTINATION reactos/system32/drivers NO_CAB FOR all)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(gpioclx PRIVATE -Wno-old-style-declaration)
endif()
