
list(APPEND SOURCE
    enable.c
    palette.c
    pointer.c
    screen.c
    surface.c
    #NV2A
    nv2a/nv2a.c
    xboxbuf.h)

add_library(xboxbuf MODULE
    ${SOURCE}
    xboxbuf.rc)

set_module_type(xboxbuf kerneldll ENTRYPOINT DrvEnableDriver 12)
add_pch(xboxbuf xboxbuf.h SOURCE)
target_link_libraries(xboxbuf libcntpr)
add_importlibs(xboxbuf win32k)
add_dependencies(xboxbuf psdk)
add_cd_file(TARGET xboxbuf DESTINATION reactos/system32 FOR all)
