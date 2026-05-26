add_executable(glxgears glxgears.c)
set_module_type(glxgears win32gui)
add_importlibs(glxgears opengl32 gdi32 user32 msvcrt kernel32)
add_cd_file(TARGET glxgears DESTINATION reactos/system32 FOR all)
