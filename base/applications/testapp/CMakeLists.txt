
list(APPEND SOURCE
    winmain.c)

add_executable(prog ${SOURCE})
set_module_type(prog win32gui UNICODE)
add_importlibs(prog advapi32 comctl32 user32 shell32 gdi32 msvcrt kernel32)
add_cd_file(TARGET prog DESTINATION reactos/system32 FOR all)
