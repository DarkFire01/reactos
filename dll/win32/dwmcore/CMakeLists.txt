
remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x601 -DWINVER=0x601)

add_definitions(-D_DWMCORE_)
spec2def(dwmcore.dll dwmcore.spec)

list(APPEND SOURCE
    DllMain.c)

add_library(dwmcore MODULE
    ${SOURCE}
    dwmcore.rc
    ${CMAKE_CURRENT_BINARY_DIR}/dwmcore.def)

set_module_type(dwmcore win32dll)
add_importlibs(dwmcore kernel32 ntdll)
add_dependencies(dwmcore psdk)
add_cd_file(TARGET dwmcore DESTINATION reactos/system32 FOR all)
