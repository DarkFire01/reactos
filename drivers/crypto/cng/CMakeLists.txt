
spec2def(cng.sys cng.spec)

list(APPEND SOURCE
    cng.c
    ${CMAKE_CURRENT_BINARY_DIR}/cng.def)

add_library(cng MODULE ${SOURCE} cng.rc)
set_module_type(cng kernelmodedriver)
add_importlibs(cng ntoskrnl hal)
add_cd_file(TARGET cng DESTINATION reactos/system32/drivers NO_CAB FOR all)
