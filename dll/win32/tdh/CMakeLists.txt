
spec2def(tdh.dll tdh.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    tdh.c
    tdh.rc
    ${CMAKE_CURRENT_BINARY_DIR}/tdh.def)

add_library(tdh MODULE ${SOURCE})

set_module_type(tdh win32dll UNICODE)
add_importlibs(tdh msvcrt kernel32 ntdll)
add_cd_file(TARGET tdh DESTINATION reactos/system32 FOR all)
