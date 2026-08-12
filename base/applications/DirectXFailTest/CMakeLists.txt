
add_executable(dxfailtest dxfailtest.c)
set_module_type(dxfailtest win32gui)
add_importlibs(dxfailtest ddraw user32 gdi32 msvcrt kernel32)
add_cd_file(TARGET dxfailtest DESTINATION reactos/system32 FOR all)
