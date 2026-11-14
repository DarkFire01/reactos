remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(-D_WIN32_WINNT=0x600)

list(APPEND SOURCE
    adapter.c
    command.c
    control.c
    datapath.c
    miniport.c
    scan.c
    tlv.c
    wdiwifi.h)

# Defines the hook NPI id, so it cannot share the precompiled header
list(APPEND PCH_SKIP_SOURCE
    driver.c)

add_library(wdiwifi MODULE
    ${SOURCE}
    ${PCH_SKIP_SOURCE}
    wdiwifi.rc)

set_module_type(wdiwifi kernelmodedriver)
target_compile_definitions(wdiwifi PRIVATE NDIS_MINIPORT_DRIVER=1 NDIS650_MINIPORT=1)
add_importlibs(wdiwifi ndis netio ntoskrnl hal)
add_pch(wdiwifi wdiwifi.h "${PCH_SKIP_SOURCE}")
add_cd_file(TARGET wdiwifi DESTINATION reactos/system32/drivers FOR all)
add_registry_inf(wdiwifi_reg.inf)
