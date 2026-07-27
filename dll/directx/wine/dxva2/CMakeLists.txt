# Source list mirrors dlls/dxva2/Makefile.in upstream; keep them in sync on resync.

# main.c's video processor uses CONDITION_VARIABLE, which the PSDK gates behind
# _WIN32_WINNT >= 0x600. The backing exports come from kernel32_vista below.
remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)

add_definitions(
    -D__WINESRC__
    -D__ROS_LONG64__
    -D_WIN32_WINNT=0x0600
    -DWINVER=0x0600)

spec2def(dxva2.dll dxva2.spec ADD_IMPORTLIB)

add_library(dxva2 MODULE
    main.c
    ${CMAKE_CURRENT_BINARY_DIR}/dxva2_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/dxva2.def)

set_module_type(dxva2 win32dll)
target_link_libraries(dxva2 wine uuid)
# user32 is a divergence from upstream's IMPORTS line. Wine's own <winuser.h>
# supplies static inline versions of the small RECT helpers (IsRectEmpty,
# SetRectEmpty and friends) whenever __WINESRC__ is defined, so upstream never
# links them; ReactOS's PSDK declares them as ordinary user32 exports.
add_importlibs(dxva2 ole32 user32 kernel32_vista msvcrt kernel32 ntdll)
# main.c includes dxva2api.h and dxvahd.h, which are generated from the IDLs in
# sdk/include/psdk rather than shipped as headers.
add_dependencies(dxva2 wineheaders psdk)
add_cd_file(TARGET dxva2 DESTINATION reactos/system32 FOR all)
set_wine_module(dxva2)
