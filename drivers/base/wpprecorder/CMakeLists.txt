
spec2def(wpprecorder.sys wpprecorder.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    wpprecorder.c
    wpprecorder.rc
    ${CMAKE_CURRENT_BINARY_DIR}/wpprecorder.def)

add_library(wpprecorder MODULE ${SOURCE})
set_module_type(wpprecorder kernelmodedriver ENTRYPOINT 0)
add_importlibs(wpprecorder ntoskrnl)
add_cd_file(TARGET wpprecorder DESTINATION reactos/system32/drivers FOR all)
