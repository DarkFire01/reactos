add_executable(gllit gllit.c)
set_module_type(gllit win32gui)
add_importlibs(gllit opengl32 gdi32 user32 msvcrt kernel32)
add_cd_file(TARGET gllit DESTINATION reactos/system32 FOR all)
