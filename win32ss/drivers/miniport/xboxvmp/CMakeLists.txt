
list(APPEND SOURCE
    xboxvmp.c
    nv2a.c
    xboxi2c.c
    #encoder
    encoder/conexant.c
    encoder/encoder.c
    xboxvmp.h)

add_library(xboxvmp MODULE
    ${SOURCE}
    xboxvmp.rc)

set_module_type(xboxvmp kernelmodedriver)
add_importlibs(xboxvmp ntoskrnl videoprt)

if(SARCH STREQUAL "xbox")
    add_cd_file(TARGET xboxvmp DESTINATION reactos/system32/drivers FOR all)
    add_registry_inf(xboxvmp_reg.inf)
    add_driver_inf(xboxvmp xboxdisp.inf)
endif()
