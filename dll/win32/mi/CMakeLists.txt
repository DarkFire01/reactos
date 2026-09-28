spec2def(mi.dll mi.spec)

list(APPEND SOURCE
    mi.c
    ${CMAKE_CURRENT_BINARY_DIR}/mi.def)

add_library(mi MODULE ${SOURCE} mi.rc)
set_module_type(mi win32dll ENTRYPOINT DllMain 12)
add_importlibs(mi kernel32 ntdll)
add_cd_file(TARGET mi DESTINATION reactos/system32 FOR all)
