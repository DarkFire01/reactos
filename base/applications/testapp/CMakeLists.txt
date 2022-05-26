
list(APPEND SOURCE
    main.c
    precomp.h)

add_executable(testapp ${SOURCE})
set_module_type(testapp win32gui UNICODE)
add_importlibs(testapp advapi32 user32 msvcrt kernel32)

if(MSVC)
    add_importlibs(testapp ntdll)
endif()

add_cd_file(TARGET testapp DESTINATION reactos/system32 FOR all)
