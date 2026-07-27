# Source list mirrors dlls/dxdiagn/Makefile.in upstream; keep them in sync on resync.

add_definitions(
    -D__WINESRC__
    -D__ROS_LONG64__)

spec2def(dxdiagn.dll dxdiagn.spec)

list(APPEND SOURCE
    container.c
    dxdiag_main.c
    provider.c)

add_library(dxdiagn MODULE
    ${SOURCE}
    dxdiagn.rc
    ${CMAKE_CURRENT_BINARY_DIR}/dxdiagn.def)

# dxdiagn.idl carries "#pragma makedep register": it is the COM registration
# data for DllRegisterServer, not a header. Must come after add_library():
# the helper appends to the existing target.
add_idl_reg_scripts(dxdiagn registry dxdiagn.idl)

set_module_type(dxdiagn win32dll)
# wine_dll_register/wine_dll_canunload supply the DllRegisterServer,
# DllUnregisterServer and DllCanUnloadNow exports declared in dxdiagn.spec;
# upstream Wine generates those from its own build glue.
# oldnames supplies the POSIX aliases provider.c uses (wcsicmp, wcsnicmp) plus
# the CRT opt-ins that declare them under MSVC.
target_link_libraries(dxdiagn wine wine_dll_register wine_dll_canunload strmiids dxguid uuid oldnames)
# psapi is the one divergence from upstream's IMPORTS line: provider.c calls
# EnumProcesses, EnumProcessModules and GetModuleBaseNameW, which Wine exports
# from its own kernel32 (as the K32* set psapi forwards to). ReactOS keeps them
# in psapi, the way Windows does.
add_importlibs(dxdiagn
    d3d9 ddraw dsound version ole32 oleaut32 user32 advapi32 psapi
    msvcrt kernel32 ntdll)
add_dependencies(dxdiagn wineheaders)
add_cd_file(TARGET dxdiagn DESTINATION reactos/system32 FOR all)
set_wine_module(dxdiagn)
