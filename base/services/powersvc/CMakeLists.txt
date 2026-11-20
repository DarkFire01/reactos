remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00)

list(APPEND SOURCE
    main.c
    scheme.c
    powersvc.h)

add_executable(powersvc ${SOURCE} powersvc.rc)
set_module_type(powersvc win32cui UNICODE)
add_importlibs(powersvc advapi32 msvcrt kernel32 ntdll)
add_cd_file(TARGET powersvc DESTINATION reactos/system32 FOR all)
