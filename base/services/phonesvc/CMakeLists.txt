remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00)

list(APPEND SOURCE
    audio.c
    calls.c
    info.c
    main.c
    mms.c
    mmsnet.c
    mmspdu.c
    pipe.c
    radio.c
    sms.c
    store.c
    wake.c
    mmspdu.h
    phonesvc.h)

add_executable(phonesvc ${SOURCE} phonesvc.rc)
target_link_libraries(phonesvc uuid)
set_module_type(phonesvc win32cui UNICODE)
add_delay_importlibs(phonesvc winhttp)
add_importlibs(phonesvc rilproxy iphlpapi setupapi advapi32 kernel32_vista msvcrt kernel32 ntdll)
add_cd_file(TARGET phonesvc DESTINATION reactos/system32 FOR all)
