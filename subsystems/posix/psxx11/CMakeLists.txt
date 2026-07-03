# psxx11.exe -- the companion X Window System server.
#
# A plain Win32 GUI program (no POSIX dependency): owns a DIB-backed window and
# listens on a named pipe, speaking the X11 wire protocol to POSIX X clients whose
# bytes psxss relays here (see psxss/xconn.c). psxx11.exe is to graphics what
# posixterm is to the terminal -- the Win32-side process that does the GDI drawing
# on behalf of a sandboxed POSIX client.
#
# LICENSE: MIT (clean-room; no GPL legacy psxx.dll code is reused).

list(APPEND SOURCE
    psxx11.c)

add_executable(psxx11 ${SOURCE})

# Win32 GUI application (IMAGE_SUBSYSTEM_WINDOWS_GUI): it owns a real window and a
# GDI framebuffer, and wants no console. UNICODE -> wWinMainCRTStartup -> wWinMain.
set_module_type(psxx11 win32gui UNICODE)

# user32/gdi32 = the window + all drawing; kernel32 = named pipe + threads + heap;
# msvcrt = mem*/str* the compiler emits; ntdll pulled in for Rtl* mem helpers.
add_importlibs(psxx11 user32 gdi32 msvcrt kernel32 ntdll)

add_cd_file(TARGET psxx11 DESTINATION reactos/system32 FOR all)
