
remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(
    -D_WIN32_WINNT=0x602
    -DNTDDI_VERSION=0x06020000) # NTDDI_WIN8

list(APPEND SOURCE
    ufshci.c
    ufsinit.c
    ufsscsi.c
    storufs.c)

add_library(storufs MODULE ${SOURCE} storufs.rc)

set_module_type(storufs kernelmodedriver)
add_importlibs(storufs storport ntoskrnl hal)
add_cd_file(TARGET storufs DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_registry_inf(storufs_reg.inf)
