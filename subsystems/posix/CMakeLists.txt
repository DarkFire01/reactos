
# POSIX+ Environment Subsystem
add_subdirectory(psxdll)    # the POSIX client DLL (psxdll.dll)
add_subdirectory(psxss)     # the subsystem server (psxss.exe)
add_subdirectory(posixterm) # controlling-terminal / session host w/ vt100.c emulator
add_subdirectory(psxx11)    # companion X Window System server (Win32 GDI ddx)

# Everything else POSIX -- the psxcrt C runtime, the reskit/BSD userland, the
# X11R6.9/Motif/CDE client stack, and the /bin + /etc runtime data -- lives in
# the sibling 'reactos_posix' repository.  Clone it NEXT TO the reactos source
# tree and it is configured and built as part of this build.  Must come after
# psxdll: its apps link the psxdll import library defined above.
get_filename_component(REACTOS_POSIX_SOURCE_DIR ${REACTOS_SOURCE_DIR}/../reactos_posix ABSOLUTE)
if(EXISTS ${REACTOS_POSIX_SOURCE_DIR}/CMakeLists.txt)
    message(STATUS "Found reactos_posix at ${REACTOS_POSIX_SOURCE_DIR} -- building the POSIX userland")
    add_subdirectory(${REACTOS_POSIX_SOURCE_DIR} ${CMAKE_BINARY_DIR}/reactos_posix)
else()
    message(STATUS "reactos_posix not found next to the source tree -- building the core POSIX subsystem only")
endif()
