add_definitions(-D_WOW64_)
spec2def(wow64.dll def/wow64.spec ADD_IMPORTLIB)

add_asm_files(wow64_asm srasm.S exception.S)
add_library(wow64 MODULE
    wow64.c
    entrypoint.c
    wine/registry.c
    wine/struct32.h
    wine/file.c
    wine/security.c
    wine/system.c
    wine/syscall.c
    wine/sync.c
    wine/virtual.c
    exception.c
    ros_wow64_private.h
    ${wow64_asm}
    ${CMAKE_CURRENT_BINARY_DIR}/wow64.def
    ${CMAKE_CURRENT_BINARY_DIR}/wow64_stubs.c)

set_module_type(wow64 win32dll UNICODE ENTRYPOINT DllMain 12)
#############################################
## HACK FOR MSVC COMPILATION WITH win32dll ##
set_subsystem(wow64 console)
################# END  HACK #################

# FIXME: force a base address below 4 GiB, this one was chosen arbitrarily
set_image_base(wow64 0xFF000000)
add_importlibs(wow64 ntdll)

add_cd_file(TARGET wow64 DESTINATION reactos/system32 NO_CAB FOR all)
