
add_executable(gnssfix gnssfix.c gnssfix.rc)
set_module_type(gnssfix win32cui UNICODE)
add_importlibs(gnssfix msvcrt kernel32)
add_cd_file(TARGET gnssfix DESTINATION reactos/system32 FOR all)
