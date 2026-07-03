
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
    syscalls.c
    psxdllp.h)

add_library(psxdll MODULE
    ${SOURCE}
    ${CMAKE_CURRENT_BINARY_DIR}/psxdll_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/psxdll.def)

# Our POSIX SDK headers (errno.h, sys/types.h, ...) plus the shared subsystem
# protocol header (subsys/posix/psxmsg.h, already on the reactos include path).
target_include_directories(psxdll PRIVATE ${REACTOS_SOURCE_DIR}/sdk/include/psx)

# A POSIX-subsystem image (IMAGE_SUBSYSTEM_POSIX_CUI = 7). 'module' avoids the
# default win32 entry/subsystem so we can set the POSIX ones explicitly.
set_module_type(psxdll module)
set_subsystem(psxdll posix)
# psxdll's own DLL entry is a benign DllMain; __PosixProcessStartup is exported
# for a POSIX image to use as ITS PE entry point (that's where the per-process
# server connect happens).
set_entrypoint(psxdll DllMain 12)

add_importlibs(psxdll ntdll)
add_pch(psxdll psxdllp.h SOURCE)
add_cd_file(TARGET psxdll DESTINATION reactos/system32 FOR all)
