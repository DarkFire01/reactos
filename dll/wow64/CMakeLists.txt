add_definitions(-D_WOW64_)
spec2def(wow64.dll def/wow64.spec ADD_IMPORTLIB)

add_asm_files(libwow64common_asm srasm.S)
add_library(libwow64common STATIC 
    ${libwow64common_asm} 
    ros_wow64_private.h 
    redirection.c 
    redirection.h)

add_library(wow64 MODULE
    entrypoint.c
    exception.c
    wow64.c
    lpc.c
    service.cpp
    wine/registry.c
    wine/struct32.h
    wine/file.c
    wine/security.c
    wine/system.c
    wine/syscall.c
    wine/sync.c
    wine/virtual.c
    csr.c
    ros_wow64_private.h
    entrypoint.h
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
target_link_libraries(wow64 libwow64common)
add_dependencies(wow64 psdk)

add_cd_file(TARGET wow64 DESTINATION reactos/system32 NO_WOW64 FOR all)
