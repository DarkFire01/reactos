
list(APPEND SOURCE
    main.c
    server.c
    handlers.c
    identity.c
    sbapi.c
    session.c
    process.c
    fd.c
    path.c
    procops.c
    signals.c
    pipe.c
    pty.c
    xconn.c
    timer.c
    psxss.h)

add_executable(psxss ${SOURCE})

# POSIX SDK headers; subsys/posix/psxmsg.h is already on the include path.
target_include_directories(psxss PRIVATE ${REACTOS_SOURCE_DIR}/sdk/include/psx)

# GUI subsystem image: uses Win32 and native LPC without a console window.
set_module_type(psxss win32gui)

# The CRT is not used: custom entry point, string helpers come from ntdll, and no
# page-sized stack buffers so __chkstk is not needed.
set_entrypoint(psxss PsxServerStartup 4)

# smlib provides SmConnectToSm().
target_link_libraries(psxss smlib)

# advapi32: LSA and token APIs. kernel32: process and thread APIs. ntdll: native
# LPC, object and Rtl APIs plus CRT string helpers.
add_importlibs(psxss advapi32 kernel32 ntdll)
add_pch(psxss psxss.h SOURCE)
add_cd_file(TARGET psxss DESTINATION reactos/system32 FOR all)
