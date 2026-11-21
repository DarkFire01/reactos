remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x600 -DWINVER=0x600)

add_executable(clock clock.c clock.rc)
set_module_type(clock win32gui UNICODE)
add_importlibs(clock winmm user32 gdi32 msvcrt kernel32)
add_cd_file(TARGET clock DESTINATION reactos/system32 FOR all)
