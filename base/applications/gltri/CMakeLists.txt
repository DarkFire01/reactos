add_executable(gltri gltri.c)
set_module_type(gltri win32gui)
add_importlibs(gltri opengl32 gdi32 user32 msvcrt kernel32)
add_cd_file(TARGET gltri DESTINATION reactos/system32 FOR all)
