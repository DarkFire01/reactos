remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)

add_definitions(
    -D__WINESRC__
    -D__ROS_LONG64__
    -D_USE_MATH_DEFINES
    -D_WIN32_WINNT=0x0600
    -DWINVER=0x0600)

spec2def(d2d1.dll d2d1.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    bitmap.c
    bitmap_render_target.c
    brush.c
    command_list.c
    dc_render_target.c
    device.c
    effect.c
    factory.c
    geometry.c
    hwnd_render_target.c
    layer.c
    mesh.c
    state_block.c
    stroke.c
    version.rc
    wic_render_target.c)

add_library(d2d1 MODULE
    ${SOURCE}
    version.rc
    ${CMAKE_CURRENT_BINARY_DIR}/d2d1_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/d2d1.def)

if(MSVC)
    # C5286: implicit conversion between enum types
    # C4133: device.c casts a D3D_FEATURE_LEVEL* to an enum wined3d_feature_level*.
    # C4098: 'void' function returning a void expression
    target_compile_options(d2d1 PRIVATE /wd5286 /wd4133 /wd4098)
endif()

set_module_type(d2d1 win32dll UNICODE)
target_link_libraries(d2d1 wine dxguid dx10guid uuid oldnames)
add_importlibs(d2d1 d3d10_1 dxguid d3dcompiler dwrite xmllite ole32 uuid gdi32 user32 advapi32 msvcrt kernel32 ntdll)
add_dependencies(d2d1 wineheaders d3d_idl_headers)
add_cd_file(TARGET d2d1 DESTINATION reactos/system32 FOR all)
set_wine_module(d2d1)
