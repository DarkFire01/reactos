# Source list mirrors dlls/qedit/Makefile.in upstream; keep them in sync on resync.

add_definitions(
    -D__WINESRC__
    -D__ROS_LONG64__
    # main.c has its own DllRegisterServer and calls the modern no-argument
    # __wine_register_resources; ReactOS keeps both shapes in rpcproxy.h behind
    # this switch.
    -DUSE_NEW_WINE_REGISTER_RESOURCES)

spec2def(qedit.dll qedit.spec)

list(APPEND SOURCE
    main.c
    mediadet.c
    nullrenderer.c
    samplegrabber.c
    timeline.c)

add_library(qedit MODULE
    ${SOURCE}
    version.rc
    ${CMAKE_CURRENT_BINARY_DIR}/qedit.def)

# qedit_classes.idl carries "#pragma makedep register": it is the COM
# registration data for DllRegisterServer, not a header. Must come after
# add_library(): the helper appends to the existing target.
add_idl_reg_scripts(qedit registry qedit_classes.idl)

set_module_type(qedit win32dll)
# main.c defines DllRegisterServer/DllUnregisterServer itself, so only the
# DllCanUnloadNow helper is needed here.
# oldnames supplies the POSIX alias mediadet.c uses (wcsdup) plus the CRT
# opt-ins that declare it under MSVC.
target_link_libraries(qedit wine wine_dll_canunload strmbase strmiids uuid oldnames)
# user32 is a divergence from upstream: strmbase mediatype.c calls
# IsRectEmpty, which Wine supplies as a static inline from its own
# <winuser.h> under __WINESRC__ while ReactOS keeps it in user32.
add_importlibs(qedit ole32 oleaut32 advapi32 user32 msvcrt kernel32 ntdll)
add_dependencies(qedit wineheaders)
add_cd_file(TARGET qedit DESTINATION reactos/system32 FOR all)
set_wine_module(qedit)
