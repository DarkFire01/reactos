
spec2def(wpprecorder.sys wpprecorder.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    wpprecorder.c
    ${CMAKE_CURRENT_BINARY_DIR}/wpprecorder.def)

add_library(wpprecorder MODULE ${SOURCE} wpprecorder.rc)
set_module_type(wpprecorder kernelmodedriver)
set_target_properties(wpprecorder PROPERTIES SUFFIX ".sys")
add_importlibs(wpprecorder ntoskrnl)
add_cd_file(TARGET wpprecorder DESTINATION reactos/system32/drivers NO_CAB FOR all)
