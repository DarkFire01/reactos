remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x600 -DWINVER=0x600)

list(APPEND SOURCE
    settings.c
    network.c
    sound.c)

add_executable(settings ${SOURCE} settings.rc)
set_module_type(settings win32gui UNICODE)
add_importlibs(settings wlanapi iphlpapi dhcpcsvc winmm version advapi32 user32 gdi32 msvcrt kernel32)
add_cd_file(TARGET settings DESTINATION reactos/system32 FOR all)
