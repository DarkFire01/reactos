
spec2def(psxdll.dll psxdll.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    init.c
    heap.c
    pwd.c
    sigjmp.c
    rtl.c
    lpc.c
    path.c
    fileio.c
    pathops.c
    signal.c
    misc.c
    process.c
    terminal.c
    dirent.c
    tty.c
    pty.c
    syscalls.c
    psxdllp.h)

add_library(psxdll MODULE
    ${SOURCE}
    ${CMAKE_CURRENT_BINARY_DIR}/psxdll_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/psxdll.def)

# POSIX SDK headers; subsys/posix/psxmsg.h is already on the include path
target_include_directories(psxdll PRIVATE ${REACTOS_SOURCE_DIR}/sdk/include/psx)

# 'module' avoids the default Win32 entry point and subsystem so the POSIX ones can be set
set_module_type(psxdll module)
set_subsystem(psxdll posix)
# DllMain connects to the server; __PosixProcessStartup is exported for POSIX images
set_entrypoint(psxdll DllMain 12)

add_importlibs(psxdll ntdll)
add_pch(psxdll psxdllp.h SOURCE)
add_cd_file(TARGET psxdll DESTINATION reactos/system32 FOR all)
