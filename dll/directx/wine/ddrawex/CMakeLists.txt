# Source list mirrors dlls/ddrawex/Makefile.in upstream; keep them in sync on resync.

add_definitions(
    -D__WINESRC__
    -D__ROS_LONG64__)

spec2def(ddrawex.dll ddrawex.spec)

list(APPEND SOURCE
    ddraw.c
    main.c
    surface.c)

add_library(ddrawex MODULE
    ${SOURCE}
    version.rc
    ${CMAKE_CURRENT_BINARY_DIR}/ddrawex.def)

# ddrawex.idl carries "#pragma makedep register": it is the COM registration
# data for DllRegisterServer, not a header. Must come after add_library():
# the helper appends to the existing target.
add_idl_reg_scripts(ddrawex registry ddrawex.idl)

set_module_type(ddrawex win32dll)
# wine_dll_register/wine_dll_canunload supply the DllRegisterServer,
# DllUnregisterServer and DllCanUnloadNow exports declared in ddrawex.spec;
# upstream Wine generates those from its own build glue.
target_link_libraries(ddrawex wine wine_dll_register wine_dll_canunload dxguid uuid)
add_importlibs(ddrawex ddraw ole32 advapi32 msvcrt kernel32 ntdll)
add_dependencies(ddrawex wineheaders)
add_cd_file(TARGET ddrawex DESTINATION reactos/system32 FOR all)
set_wine_module(ddrawex)
