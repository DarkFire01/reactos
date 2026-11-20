
add_library(acpitime MODULE
    acpitime.c
    acpitime.rc)

set_module_type(acpitime kernelmodedriver)
add_importlibs(acpitime ntoskrnl hal)
add_cd_file(TARGET acpitime DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_registry_inf(acpitime_reg.inf)
