add_executable(reactvm reactvm.c reactvm.rc)
set_module_type(reactvm win32cui)
add_importlibs(reactvm advapi32 msvcrt kernel32)
add_cd_file(TARGET reactvm DESTINATION reactos/system32 FOR all)
