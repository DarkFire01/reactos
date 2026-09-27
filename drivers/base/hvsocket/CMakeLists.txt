spec2def(hvsocket.sys hvsocket.spec)

list(APPEND SOURCE
    hvsocket.c
    ${CMAKE_CURRENT_BINARY_DIR}/hvsocket.def)

add_library(hvsocket MODULE ${SOURCE} hvsocket.rc)
set_module_type(hvsocket kernelmodedriver)
add_importlibs(hvsocket ntoskrnl)
add_cd_file(TARGET hvsocket DESTINATION reactos/system32/drivers FOR all)
