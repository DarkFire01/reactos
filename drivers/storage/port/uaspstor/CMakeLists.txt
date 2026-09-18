#
# The USB client library and the storport virtual miniport interface both
# appeared after the tree default, so that default has to come off first.
#
remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(
    -D_WIN32_WINNT=0x602
    -DNTDDI_VERSION=0x06020000) # NTDDI_WIN8

list(APPEND SOURCE
    guid.c
    uasdev.c
    uasio.c
    uasqueue.c
    uasurb.c
    uaspstor.c)

add_library(uaspstor MODULE ${SOURCE} uaspstor.rc)

set_module_type(uaspstor kernelmodedriver)
add_importlibs(uaspstor storport usbd ntoskrnl hal)
target_link_libraries(uaspstor usbdex)
add_cd_file(TARGET uaspstor DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_registry_inf(uaspstor_reg.inf)
