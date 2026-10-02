spec2def(wer.dll wer.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    wer.c
    ${CMAKE_CURRENT_BINARY_DIR}/wer.def)

add_library(wer MODULE ${SOURCE} wer.rc)
set_module_type(wer win32dll ENTRYPOINT DllMain 12)
add_importlibs(wer kernel32 ntdll)
add_cd_file(TARGET wer DESTINATION reactos/system32 FOR all)
