
list(APPEND SOURCE
    ioctl.c
    kbdhid.c
    pnp.c
    read.c
    wmi.c
    kbdhid.h)

add_library(kbdhid MODULE ${SOURCE} kbdhid.rc)
set_module_type(kbdhid kernelmodedriver)
add_importlibs(kbdhid ntoskrnl hal hidparse wmilib)
add_pch(kbdhid kbdhid.h SOURCE)
add_cd_file(TARGET kbdhid DESTINATION reactos/system32/drivers NO_CAB FOR all)
