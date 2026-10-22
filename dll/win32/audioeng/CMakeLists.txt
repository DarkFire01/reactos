
spec2def(audioeng.dll audioeng.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    audioeng.c
    ${CMAKE_CURRENT_BINARY_DIR}/audioeng.def)

add_library(audioeng MODULE ${SOURCE} audioeng.rc)
set_module_type(audioeng win32dll)
add_importlibs(audioeng msvcrt kernel32 ntdll)
add_cd_file(TARGET audioeng DESTINATION reactos/system32 FOR all)
