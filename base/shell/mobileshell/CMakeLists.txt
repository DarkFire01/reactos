
remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x600 -DWINVER=0x600)

list(APPEND SOURCE
    main.c
    battery.c
    keyboard.c
    mobileshell.h)

add_executable(mobileshell ${SOURCE} mobileshell.rc)
set_module_type(mobileshell win32gui UNICODE)
add_importlibs(mobileshell setupapi user32 gdi32 msvcrt kernel32)
add_cd_file(TARGET mobileshell DESTINATION reactos/system32 FOR all)
