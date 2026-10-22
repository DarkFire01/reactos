
spec2def(tbs.dll tbs.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    tbs.c
    ${CMAKE_CURRENT_BINARY_DIR}/tbs.def)

add_library(tbs MODULE ${SOURCE} tbs.rc)
set_module_type(tbs win32dll)
add_importlibs(tbs msvcrt kernel32 ntdll)
add_cd_file(TARGET tbs DESTINATION reactos/system32 FOR all)
