# SpbCx: the KMDF class extension an I2C, SPI or UART controller driver binds to.
# The controller driver owns the hardware; this owns the target a peripheral
# opens, the request queue, and turning the <spb.h> IOCTLs into typed callbacks.
# A KMDF client, so the tree default has to come off before wdfdriverentry
# publishes the higher NTDDI floor the KMDF headers need.
remove_definitions(-D_WIN32_WINNT=0x502)

list(APPEND SOURCE
    capture.c
    controller.c
    request.c
    spbcx.c
    target.c
    spbcxp.h)

add_library(spbcx MODULE ${SOURCE} spbcx.rc)
set_module_type(spbcx kmdfdriver)
add_importlibs(spbcx ntoskrnl hal)

# capture.c probes and locks caller pages under SEH
target_link_libraries(spbcx pseh)

target_include_directories(spbcx PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/kmdf/inc/private
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/shared/inc/private/common)

add_cd_file(TARGET spbcx DESTINATION reactos/system32/drivers NO_CAB FOR all)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(spbcx PRIVATE -Wno-old-style-declaration)
endif()
