
# POSIX+ Environment Subsystem
add_subdirectory(psxdll)    # the POSIX client DLL (psxdll.dll)
add_subdirectory(psxss)     # the subsystem server (psxss.exe)
add_subdirectory(posixterm) # controlling-terminal / session host w/ vt100.c emulator
add_subdirectory(psxx11)    # companion X Window System server (Win32 GDI ddx)
