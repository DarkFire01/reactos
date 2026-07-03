
# POSIX+ Environment Subsystem
add_subdirectory(psxdll)    # POSIX client DLL
add_subdirectory(psxss)     # Subsystem server
add_subdirectory(posixterm) # Terminal and session host with VT100 emulation
add_subdirectory(psxx11)    # X Window System server

# The POSIX userland (C runtime, utilities, X11 clients, /bin and /etc data) lives in the
# reactos_posix repository, cloned next to the reactos source tree. It must come after psxdll.
get_filename_component(REACTOS_POSIX_SOURCE_DIR ${REACTOS_SOURCE_DIR}/../reactos_posix ABSOLUTE)
if(EXISTS ${REACTOS_POSIX_SOURCE_DIR}/CMakeLists.txt)
    message(STATUS "Found reactos_posix at ${REACTOS_POSIX_SOURCE_DIR}, building the POSIX userland")
    add_subdirectory(${REACTOS_POSIX_SOURCE_DIR} ${CMAKE_BINARY_DIR}/reactos_posix)
else()
    message(STATUS "reactos_posix not found next to the source tree, building the core POSIX subsystem only")
endif()
