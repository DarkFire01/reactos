
remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x600 -DWINVER=0x600)

add_definitions(-D_DWMREDIR_)
spec2def(dwmredir.dll dwmredir.spec)

list(APPEND SOURCE
    DllMain.c)

add_library(dwmredir MODULE
    ${SOURCE}
    dwmredir.rc
    ${CMAKE_CURRENT_BINARY_DIR}/dwmredir.def
    ${CMAKE_CURRENT_BINARY_DIR}/dwmredir_stubs.c)

set_module_type(dwmredir win32dll ENTRYPOINT DllMain)
add_importlibs(dwmredir kernel32 ntdll)
add_dependencies(dwmredir psdk)
add_cd_file(TARGET dwmredir DESTINATION reactos/system32 FOR all)
