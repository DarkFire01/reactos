
# Source list mirrors dlls/dxgi/Makefile.in upstream.

# swapchain.c uses CONDITION_VARIABLE, which the PSDK gates behind
# _WIN32_WINNT >= 0x600. kernel32/ntdll export the backing functions and they
# are present in the import libs at this tree's DLL_EXPORT_VERSION.
remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)

add_definitions(
    -D__WINESRC__
    -D__ROS_LONG64__
    -D_WIN32_WINNT=0x0600
    -DWINVER=0x0600)

include_directories(${VKD3D_INCLUDE_DIRS})

spec2def(dxgi.dll dxgi.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    adapter.c
    device.c
    dxgi_main.c
    factory.c
    output.c
    resource.c
    swapchain.c
    utils.c)

add_library(dxgi MODULE
    ${SOURCE}
    version.rc
    ${CMAKE_CURRENT_BINARY_DIR}/dxgi_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/dxgi.def)

if(MSVC)
    target_compile_options(dxgi PRIVATE /wd5286)
endif()

set_module_type(dxgi win32dll UNICODE)
# dx10guid carries the IDXGI*/ID3D10*/ID3D12* IIDs (dxguid only has the
# pre-D3D10 set); both are static libraries.
target_link_libraries(dxgi wine dxguid dx10guid uuid)
add_importlibs(dxgi d3dwine gdi32 user32 win32u msvcrt kernel32 ntdll)
add_dependencies(dxgi wineheaders d3d_idl_headers)
add_cd_file(TARGET dxgi DESTINATION reactos/system32 FOR all)
set_wine_module(dxgi)
