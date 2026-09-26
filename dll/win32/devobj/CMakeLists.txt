
spec2def(devobj.dll devobj.spec)

list(APPEND SOURCE
    devobj.rc
    ${CMAKE_CURRENT_BINARY_DIR}/devobj_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/devobj.def)

add_library(devobj MODULE ${SOURCE})

set_module_type(devobj win32dll)
add_importlibs(devobj setupapi msvcrt kernel32 ntdll)
add_cd_file(TARGET devobj DESTINATION reactos/system32 FOR all)
