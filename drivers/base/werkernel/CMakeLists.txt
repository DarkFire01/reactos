
spec2def(ext-ms-win-ntos-werkernel-l1-1-1.dll werkernel.spec)

list(APPEND SOURCE
    werkernel.c
    ${CMAKE_CURRENT_BINARY_DIR}/ext-ms-win-ntos-werkernel-l1-1-1.def)

add_library(werkernel MODULE ${SOURCE} werkernel.rc)
set_module_type(werkernel kernelmodedriver)
set_target_properties(werkernel PROPERTIES OUTPUT_NAME "ext-ms-win-ntos-werkernel-l1-1-1" SUFFIX ".dll")
add_importlibs(werkernel ntoskrnl)
add_cd_file(TARGET werkernel DESTINATION reactos/system32/drivers NO_CAB FOR all)
