add_executable(gltexcube gltexcube.c)
set_module_type(gltexcube win32gui)
add_importlibs(gltexcube opengl32 gdi32 user32 msvcrt kernel32)
add_cd_file(TARGET gltexcube DESTINATION reactos/system32 FOR all)
