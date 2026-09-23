
spec2def(cea.sys cea.spec)

list(APPEND SOURCE
    cea.c
    ${CMAKE_CURRENT_BINARY_DIR}/cea.def)

add_library(cea MODULE ${SOURCE} cea.rc)
set_module_type(cea kernelmodedriver)
add_importlibs(cea ntoskrnl)
add_cd_file(TARGET cea DESTINATION reactos/system32/drivers FOR all)
