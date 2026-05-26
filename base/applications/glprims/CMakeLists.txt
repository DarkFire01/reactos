add_executable(glprims glprims.c)
set_module_type(glprims win32gui)
add_importlibs(glprims opengl32 gdi32 user32 msvcrt kernel32)
add_cd_file(TARGET glprims DESTINATION reactos/system32 FOR all)
