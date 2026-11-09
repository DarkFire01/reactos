remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x600 -DWINVER=0x600)

add_executable(phonetest phonetest.c phonetest.rc)
set_module_type(phonetest win32gui UNICODE)
add_importlibs(phonetest phoneom user32 gdi32 msvcrt kernel32)
add_cd_file(TARGET phonetest DESTINATION reactos/system32 FOR all)
