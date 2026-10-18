
add_executable(glcube glcube.c glcube.rc)
set_module_type(glcube win32gui UNICODE)
add_importlibs(glcube opengl32 user32 gdi32 msvcrt kernel32)
add_cd_file(TARGET glcube DESTINATION reactos/system32 FOR all)
