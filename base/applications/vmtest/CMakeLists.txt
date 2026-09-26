
add_executable(vmtest vmtest.c vmtest.rc)
set_module_type(vmtest win32cui)
add_importlibs(vmtest msvcrt kernel32)
add_cd_file(TARGET vmtest DESTINATION reactos/system32 FOR all)
