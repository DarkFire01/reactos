
remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x601 -DWINVER=0x601)

add_executable(mobileshell main.c mobileshell.rc)
set_module_type(mobileshell win32gui UNICODE)
add_importlibs(mobileshell user32 gdi32 msvcrt kernel32)
add_cd_file(TARGET mobileshell DESTINATION reactos/system32 FOR all)
