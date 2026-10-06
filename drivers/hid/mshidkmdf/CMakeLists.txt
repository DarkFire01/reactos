
list(APPEND SOURCE
    mshidkmdf.c
    mshidkmdf.h)

add_library(mshidkmdf MODULE ${SOURCE} mshidkmdf.rc)
set_module_type(mshidkmdf kernelmodedriver)
add_importlibs(mshidkmdf hidclass ntoskrnl hal)
add_cd_file(TARGET mshidkmdf DESTINATION reactos/system32/drivers NO_CAB FOR all)
