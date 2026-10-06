# The ACPI Resource Hub, which turns a connection id back into the controller
# that produced it. A KMDF client, so the tree default has to come off before
# wdfdriverentry publishes the higher NTDDI floor the KMDF headers need.
remove_definitions(-D_WIN32_WINNT=0x502)

list(APPEND SOURCE
    connection.c
    create.c
    driver.c
    ioctl.c
    provider.c
    translate.c
    rhpriv.h)

add_library(acpiex MODULE ${SOURCE} acpiex.rc)
set_module_type(acpiex kmdfdriver)
add_importlibs(acpiex ntoskrnl hal)

target_include_directories(acpiex PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/kmdf/inc/private
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/shared/inc/private/common)

add_cd_file(TARGET acpiex DESTINATION reactos/system32/drivers NO_CAB FOR all)

# The KMDF headers put the return type ahead of FORCEINLINE, which expands to
# "extern __inline__" under GCC and trips -Wold-style-declaration.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(acpiex PRIVATE -Wno-old-style-declaration)
endif()
