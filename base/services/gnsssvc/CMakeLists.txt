
remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00)

list(APPEND SOURCE
    codec.c
    loc.c
    main.c
    pipe.c
    router.c
    gnsssvc.h)

add_executable(gnsssvc ${SOURCE} gnsssvc.rc)
set_module_type(gnsssvc win32cui UNICODE)
add_importlibs(gnsssvc advapi32 kernel32_vista msvcrt kernel32 ntdll)
add_cd_file(TARGET gnsssvc DESTINATION reactos/system32 FOR all)
