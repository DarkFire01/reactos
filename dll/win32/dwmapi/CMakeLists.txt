
remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x600 -DWINVER=0x600)

add_definitions(-D_DWMAPI_)
spec2def(dwmapi.dll dwmapi.spec)

list(APPEND SOURCE
    DllMain.c)

add_library(dwmapi MODULE
    ${SOURCE}
    dwmapi.rc
    ${CMAKE_CURRENT_BINARY_DIR}/dwmapi.def)

set_module_type(dwmapi win32dll)
add_importlibs(dwmapi kernel32 ntdll)
add_dependencies(dwmapi psdk)
add_cd_file(TARGET dwmapi DESTINATION reactos/system32 FOR all)