
add_executable(memapp memapp.c memapp.rc)
set_module_type(memapp win32gui UNICODE)
add_importlibs(memapp user32 gdi32 msvcrt kernel32)
add_cd_file(TARGET memapp DESTINATION reactos/system32 FOR all)
