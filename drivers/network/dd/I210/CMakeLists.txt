
add_definitions(
    -DNDIS50_MINIPORT
    -DNDIS_MINIPORT_DRIVER
    -DNDIS_LEGACY_MINIPORT)

list(APPEND SOURCE
    ndis.c
    hardware.c
    info.c
    interrupt.c
    nic.h
    send.c)

add_library(i21x MODULE ${SOURCE} i21x.rc)
add_pch(i21x nic.h SOURCE)
set_module_type(i21x kernelmodedriver)
add_importlibs(i21x ndis ntoskrnl hal)
add_cd_file(TARGET i21x DESTINATION reactos/system32/drivers FOR all)
add_driver_inf(i21x neti21x.inf)
