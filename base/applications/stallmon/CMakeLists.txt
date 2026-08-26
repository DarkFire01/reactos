
# SRWLOCK, CONDITION_VARIABLE and MsgWaitForMultipleObjectsEx are Vista+, and
# the headers hide them below that. This models Chromium, whose base::Lock is
# an SRWLOCK and whose message pump waits on the queue and an event together,
# so the whole point of the program is the Vista+ surface.
remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(-D_WIN32_WINNT=0x600)

add_executable(stallmon stallmon.c stallmon.rc)
set_module_type(stallmon win32cui)
add_importlibs(stallmon user32 gdi32 msvcrt kernel32 ntdll)
add_cd_file(TARGET stallmon DESTINATION reactos/system32 FOR all)
