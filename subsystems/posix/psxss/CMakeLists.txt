
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

# Our POSIX SDK headers; subsys/posix/psxmsg.h is already on the reactos path.
target_include_directories(psxss PRIVATE ${REACTOS_SOURCE_DIR}/sdk/include/psx)

# The real NT 4.0 psxss.exe is an IMAGE_SUBSYSTEM_WINDOWS_GUI image (it uses
# Win32 + native LPC and wants no console window).
set_module_type(psxss win32gui)

# Faithful to the real NT 4.0 psxss, which statically linked its CRT (so it
# imports ntdll/kernel32/advapi32, NOT msvcrt). We use a custom entry point
# instead of the CRT's _WinMainCRTStartup, take str*/_snwprintf from ntdll's
# exports, and avoid __chkstk by not putting page-sized buffers on the stack.
set_entrypoint(psxss PsxServerStartup 4)

# smlib provides SmConnectToSm() (the SM registration client stubs), same as csrsrv.
target_link_libraries(psxss smlib)

# advapi32 = LSA + token APIs; kernel32 = OpenProcess/CloseHandle; ntdll = the
# native LPC/object/Rtl surface + the CRT string helpers.
add_importlibs(psxss advapi32 kernel32 ntdll)
add_pch(psxss psxss.h SOURCE)
add_cd_file(TARGET psxss DESTINATION reactos/system32 FOR all)
