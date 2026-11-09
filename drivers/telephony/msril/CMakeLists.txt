
list(APPEND SOURCE
    msril.c
    msril.h)

add_library(msril MODULE ${SOURCE} msril.rc)
set_module_type(msril wdmdriver)
add_importlibs(msril ntoskrnl hal)
add_cd_file(TARGET msril DESTINATION reactos/system32/drivers FOR all)
