
list(APPEND SOURCE
    mshidkmdf.c
    mshidkmdf.h)

add_library(mshidkmdf MODULE ${SOURCE} mshidkmdf.rc)
set_module_type(mshidkmdf kernelmodedriver)
add_importlibs(mshidkmdf hidclass ntoskrnl)
add_cd_file(TARGET mshidkmdf DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_registry_inf(mshidkmdf_reg.inf)
