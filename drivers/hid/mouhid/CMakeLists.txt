
list(APPEND SOURCE
    create.c
    mouhid.c
    pnp.c
    read.c
    wmi.c
    mouhid.h)

add_library(mouhid MODULE ${SOURCE} mouhid.rc)
set_module_type(mouhid kernelmodedriver)
add_importlibs(mouhid ntoskrnl hal hidparse wmilib)
add_cd_file(TARGET mouhid DESTINATION reactos/system32/drivers FOR all)
