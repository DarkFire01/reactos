# posixterm.exe -- the POSIX controlling-terminal / session host.
#
# A drop-in alternative to the real NT 4.0 posix.exe: a Win32 console program
# that registers as the session leader with psxss (\PSXSS\SESPORT), creates its
# own per-session data port + section (\PSXSS\PSXSES\P<pid> / D<pid>), asks psxss
# to spawn the POSIX child (default: /bin/sh), and then services that child's
# tty read/write/isatty/tcsetattr/exit over LPC -- driving the Win32 console
# through John Miller's permissive vt100.c/console.c VT100 emulator.
#
# LICENSE: the bridge (posixterm.c) is MIT; vt100.c/console.c are John L. Miller's
# permissive terminal emulator (see the header in vt100.h). No GPL code is used
# (the legacy GPL posixw32.c skeleton is NOT reused, only the emulator core).

list(APPEND SOURCE
    posixterm.c
    vt100.c
    console.c)

add_executable(posixterm ${SOURCE})

target_include_directories(posixterm PRIVATE
    ${REACTOS_SOURCE_DIR}/sdk/include/psx)

# Win32 console application (IMAGE_SUBSYSTEM_WINDOWS_CUI), exactly like the real
# posix.exe -- it drives CONIN$/CONOUT$ and wants a console window.
set_module_type(posixterm win32cui)

# smlib = the Session Manager client stubs (SmConnectToSm/SmLoadDeferedSubsystem)
# used to cold-load the POSIX subsystem when it isn't already running.
target_link_libraries(posixterm smlib)

# msvcrt = the C runtime (vsprintf/strlen used by vt100.c) + mainCRTStartup;
# ntdll = the native LPC/object/section surface (Nt*/Rtl*); kernel32 = the
# console API (ReadConsoleInput/WriteConsole/CreateThread).
add_importlibs(posixterm msvcrt user32 kernel32 ntdll)

add_cd_file(TARGET posixterm DESTINATION reactos/system32 FOR all)
