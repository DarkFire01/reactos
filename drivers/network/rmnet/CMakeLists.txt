remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(-D_WIN32_WINNT=0x601 -DNTDDI_VERSION=0x06010000)

list(APPEND SOURCE
    control.c
    datapath.c
    ipa.c
    miniport.c
    rmnet.h)

add_library(rmnet MODULE ${SOURCE} rmnet.rc)
set_module_type(rmnet kernelmodedriver)
target_compile_definitions(rmnet PRIVATE NDIS_MINIPORT_DRIVER=1 NDIS620_MINIPORT=1)
add_importlibs(rmnet ndis ntoskrnl hal)
add_pch(rmnet rmnet.h SOURCE)
add_cd_file(TARGET rmnet DESTINATION reactos/system32/drivers FOR all)
add_registry_inf(rmnet_reg.inf)
