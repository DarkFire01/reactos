
remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(-D_WIN32_WINNT=0x600)

list(APPEND SOURCE
    control.c
    nwifi.c
    xlate.c
    nwifi.h)

add_library(nwifi MODULE
    ${SOURCE}
    nwifi.rc)

set_module_type(nwifi kernelmodedriver)
target_compile_definitions(nwifi PRIVATE NDIS_MINIPORT_DRIVER=1 NDIS650_MINIPORT=1)
add_importlibs(nwifi ndis ntoskrnl hal)
add_pch(nwifi nwifi.h SOURCE)
add_cd_file(TARGET nwifi DESTINATION reactos/system32/drivers FOR all)
add_registry_inf(nwifi_reg.inf)
