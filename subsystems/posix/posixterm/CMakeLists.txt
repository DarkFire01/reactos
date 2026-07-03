# POSIX session leader and controlling terminal host.
# posixterm.c is MIT; vt100.c and console.c carry John L. Miller's permissive license (see vt100.h).

list(APPEND SOURCE
    posixterm.c
    vt100.c
    console.c)

add_executable(posixterm ${SOURCE})

target_include_directories(posixterm PRIVATE
    ${REACTOS_SOURCE_DIR}/sdk/include/psx)

set_module_type(posixterm win32cui)

# smlib provides SmConnectToSm and SmLoadDeferedSubsystem to load psxss on demand
target_link_libraries(posixterm smlib)
add_importlibs(posixterm msvcrt user32 kernel32 ntdll)

add_cd_file(TARGET posixterm DESTINATION reactos/system32 FOR all)
