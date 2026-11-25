
remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502 -DDXGKDDI_INTERFACE_VERSION=0x1052)
add_definitions(-D_WIN32_WINNT=0xA00 -DWINVER=0xA00 -DDXGKDDI_INTERFACE_VERSION=DXGKDDI_INTERFACE_VERSION_WDDM2_4)

spec2def(opencl.dll opencl.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    dispatch.c
    extfunc.c
    icdenum.c
    opencl.c
    precomp.h)

add_library(opencl MODULE
    ${SOURCE}
    opencl.rc
    ${CMAKE_CURRENT_BINARY_DIR}/opencl.def)

set_module_type(opencl win32dll)
target_link_libraries(opencl dxguid)
add_importlibs(opencl cfgmgr32 ole32 advapi32 msvcrt kernel32 ntdll)
add_dependencies(opencl psdk)
add_pch(opencl precomp.h SOURCE)
add_cd_file(TARGET opencl DESTINATION reactos/system32 FOR all)
