list(APPEND SOURCE
    genericframebuffer.c
    genericframebuffer.h)

add_library(GenericFramebuffer MODULE ${SOURCE} genericframebuffer.rc)
set_module_type(GenericFramebuffer kernelmodedriver)
add_importlibs(GenericFramebuffer ntoskrnl videoprt)
target_link_libraries(GenericFramebuffer libcntpr)
add_pch(GenericFramebuffer genericframebuffer.h SOURCE)
add_cd_file(TARGET GenericFramebuffer DESTINATION reactos/system32/drivers FOR all)
add_registry_inf(genericframebuffer_reg.inf)
add_driver_inf(GenericFramebuffer genericframebuffer.inf)
