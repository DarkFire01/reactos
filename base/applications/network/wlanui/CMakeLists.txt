
list(APPEND SOURCE
    wlanui.c
    resource.h)

add_executable(wlanui ${SOURCE} wlanui.rc)
set_module_type(wlanui win32gui UNICODE)
add_importlibs(wlanui wlanapi user32 comctl32 advapi32 msvcrt kernel32)
add_cd_file(TARGET wlanui DESTINATION reactos/system32 FOR all)
