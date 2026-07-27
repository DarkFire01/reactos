# Source list mirrors dlls/d3dx10_43/Makefile.in upstream; keep them in sync on
# resync. d3dx10_33..42 are forwarder DLLs built on top of this one -- see
# d3dx10_forwarders.cmake next to this file.

# async.c's thread pump uses SRWLOCK and CONDITION_VARIABLE, which the PSDK
# gates behind _WIN32_WINNT >= 0x600. The backing exports are reached through
# kernel32_vista below; this only opens up the declarations. Same treatment as
# sdk/lib/vkd3d.
remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)

add_definitions(
    -D__WINESRC__
    -D__ROS_LONG64__
    -D_WIN32_WINNT=0x0600
    -DWINVER=0x0600)

include_directories(
    BEFORE
    ${REACTOS_SOURCE_DIR}/sdk/include/wine
    ${REACTOS_BINARY_DIR}/sdk/include/wine)

spec2def(d3dx10_43.dll d3dx10_43.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    async.c
    compiler.c
    d3dx10_43_main.c
    font.c
    mesh.c
    sprite.c
    texture.c
    # ReactOS addition, see the file for why.
    guid.c)

add_library(d3dx10_43 MODULE
    ${SOURCE}
    version.rc
    ${CMAKE_CURRENT_BINARY_DIR}/d3dx10_43_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/d3dx10_43.def)

set_module_type(d3dx10_43 win32dll)
# dx10guid carries the D3D10/D3DX10 IIDs (IID_ID3D10Device1, IID_ID3DX10Font
# and the rest); plain dxguid stops at D3D9.
target_link_libraries(d3dx10_43 wine dxguid dx10guid uuid)
# kernel32_vista for the SRWLOCK and condition-variable set async.c's thread
# pump is built on; the plain kernel32 importlib is trimmed to
# DLL_EXPORT_VERSION and does not carry them.
# Upstream's "d3dcompiler" import is d3dcompiler_47's here.
# d3dx9_36 is not an upstream IMPORTS entry either: the whole D3DXMath block
# of this .spec (D3DXMatrixMultiply, D3DXColorAdjustContrast and the rest) is
# a set of cross-DLL forwarders to it, which MSVC resolves against that
# import library at link time. GNU ld takes them from the .def instead.
add_importlibs(d3dx10_43
    d3d10_1 d3dcompiler_47 d3dx9_36 gdi32
    kernel32_vista ntdll_vista msvcrt kernel32 ntdll)
# texture.c reaches for WIC only when asked to load an image, and upstream
# delay-loads it for exactly that reason.
add_delay_importlibs(d3dx10_43 windowscodecs)
add_dependencies(d3dx10_43 wineheaders d3d_idl_headers)
add_cd_file(TARGET d3dx10_43 DESTINATION reactos/system32 FOR all)
