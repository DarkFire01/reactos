# Source list mirrors dlls/devenum/Makefile.in upstream; keep them in sync on resync.

remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(
    -D__WINESRC__
    -D__ROS_LONG64__
    -D_WIN32_WINNT=0x600
    -DWINVER=0x600
    # devenum_main.c has its own DllRegisterServer and calls the modern
    # no-argument __wine_register_resources; ReactOS keeps both shapes in
    # rpcproxy.h behind this switch (mscoree is the other user).
    -DUSE_NEW_WINE_REGISTER_RESOURCES)

spec2def(devenum.dll devenum.spec)

list(APPEND SOURCE
    createdevenum.c
    devenum_main.c
    mediacatenum.c
    parsedisplayname.c)

add_library(devenum MODULE
    ${SOURCE}
    devenum.rc
    ${CMAKE_CURRENT_BINARY_DIR}/devenum.def)

# devenum_classes.idl carries "#pragma makedep register": it is the COM
# registration data for DllRegisterServer, not a header. Must come after
# add_library(): the helper appends to the existing target.
add_idl_reg_scripts(devenum registry devenum_classes.idl)

set_module_type(devenum win32dll UNICODE)
# wine_dll_register/wine_dll_canunload supply the DllRegisterServer,
# DllUnregisterServer and DllCanUnloadNow exports declared in devenum.spec.
# devenum_main.c defines DllRegisterServer/DllUnregisterServer itself, so
# only the DllCanUnloadNow helper is needed here.
# oldnames supplies the POSIX aliases used here (wcsicmp, wcsdup) plus the
# CRT opt-ins that declare them under MSVC.
target_link_libraries(devenum wine wine_dll_canunload dmoguids strmiids uuid oldnames)
add_delay_importlibs(devenum msvfw32)
add_importlibs(devenum
    ole32 oleaut32 avicap32 winmm user32 advapi32 dsound msdmo
    msvcrt kernel32 ntdll)
add_dependencies(devenum wineheaders)
add_cd_file(TARGET devenum DESTINATION reactos/system32 FOR all)
set_wine_module(devenum)
