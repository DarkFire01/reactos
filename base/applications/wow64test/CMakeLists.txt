
list(APPEND SOURCE
    test.c)

add_executable(wow64test ${SOURCE})
set_module_type(wow64test win32cui UNICODE)
add_importlibs(wow64test ntdll kernel32 user32 msvcrt)
add_cd_file(TARGET wow64test DESTINATION reactos/system32 FOR all)
