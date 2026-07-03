# X Window System server for POSIX X clients. A Win32 GUI program that draws into a DIB
# backed window and serves the X11 protocol over a named pipe relayed by psxss.

list(APPEND SOURCE
    psxx11.c)

add_executable(psxx11 ${SOURCE})
set_module_type(psxx11 win32gui UNICODE)
add_importlibs(psxx11 user32 gdi32 msvcrt kernel32 ntdll)
add_cd_file(TARGET psxx11 DESTINATION reactos/system32 FOR all)
