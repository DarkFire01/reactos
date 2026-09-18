
remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(
    -D_WIN32_WINNT=0x602
    -DNTDDI_VERSION=0x06020000) # NTDDI_WIN8

list(APPEND SOURCE
    nvmectrl.c
    stornvme.c)

add_library(stornvme MODULE ${SOURCE} stornvme.rc)

set_module_type(stornvme kernelmodedriver)
add_importlibs(stornvme storport ntoskrnl hal)
add_cd_file(TARGET stornvme DESTINATION reactos/system32/drivers NO_CAB FOR all)
