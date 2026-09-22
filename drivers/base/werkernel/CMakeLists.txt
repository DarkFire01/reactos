
spec2def(werkernel.sys werkernel.spec)

list(APPEND SOURCE
    werkernel.c
    ${CMAKE_CURRENT_BINARY_DIR}/werkernel.def)

add_library(werkernel MODULE ${SOURCE} werkernel.rc)
set_module_type(werkernel kernelmodedriver)
set_target_properties(werkernel PROPERTIES SUFFIX ".sys")
add_importlibs(werkernel ntoskrnl)
add_cd_file(TARGET werkernel DESTINATION reactos/system32/drivers NO_CAB FOR all)
