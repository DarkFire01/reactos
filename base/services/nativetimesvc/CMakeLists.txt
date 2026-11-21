remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00)

list(APPEND SOURCE
    clock.c
    main.c
    pipe.c
    power.c
    triggers.c
    nativetimesvc.h)

add_executable(nativetimesvc ${SOURCE} nativetimesvc.rc)
set_module_type(nativetimesvc win32cui UNICODE)
add_importlibs(nativetimesvc rilproxy advapi32 kernel32_vista msvcrt kernel32 ntdll)
add_cd_file(TARGET nativetimesvc DESTINATION reactos/system32 FOR all)
