remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x600 -DWINVER=0x600)

add_executable(messages messages.c messages.rc)
set_module_type(messages win32gui UNICODE)
add_importlibs(messages phoneom gdiplus comdlg32 user32 gdi32 msvcrt kernel32)
if(ARCH STREQUAL "i386")
    # 64-bit math helpers such as _allmul
    add_importlibs(messages ntdll)
endif()
add_cd_file(TARGET messages DESTINATION reactos/system32 FOR all)
