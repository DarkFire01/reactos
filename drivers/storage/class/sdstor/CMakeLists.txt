
list(APPEND SOURCE
    sdstor.c
    pnp.c
    scsi.c
    sdcard.c
    precomp.h)

add_library(sdstor MODULE
    ${SOURCE}
    sdstor.rc)

add_pch(sdstor precomp.h SOURCE)
set_module_type(sdstor kernelmodedriver)
add_importlibs(sdstor ntoskrnl hal)
add_cd_file(TARGET sdstor DESTINATION reactos/system32/drivers NO_CAB FOR all)
