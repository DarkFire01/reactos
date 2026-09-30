spec2def(wevtapi.dll wevtapi.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    wevtapi.c
    ${CMAKE_CURRENT_BINARY_DIR}/wevtapi.def)

add_library(wevtapi MODULE ${SOURCE} wevtapi.rc)
set_module_type(wevtapi win32dll ENTRYPOINT DllMain 12)
add_importlibs(wevtapi kernel32 ntdll)
add_cd_file(TARGET wevtapi DESTINATION reactos/system32 FOR all)
