remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00)

list(APPEND SOURCE
    audio.c
    calls.c
    main.c
    pipe.c
    radio.c
    sms.c
    store.c
    phonesvc.h)

add_executable(phonesvc ${SOURCE} phonesvc.rc)
target_link_libraries(phonesvc uuid)
set_module_type(phonesvc win32cui UNICODE)
add_importlibs(phonesvc rilproxy setupapi shell32 advapi32 kernel32_vista msvcrt kernel32 ntdll)
add_cd_file(TARGET phonesvc DESTINATION reactos/system32 FOR all)
