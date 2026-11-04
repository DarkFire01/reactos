
add_library(j5touch MODULE j5touch.c j5touch.rc)
set_module_type(j5touch kernelmodedriver)
add_importlibs(j5touch hidclass ntoskrnl hal)
add_cd_file(TARGET j5touch DESTINATION reactos/system32/drivers FOR all)
add_driver_inf(j5touch j5touch.inf)
add_registry_inf(j5touch_reg.inf)
