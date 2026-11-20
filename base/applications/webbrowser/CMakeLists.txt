
remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x600 -DWINVER=0x600)

add_subdirectory(engine)

list(APPEND SOURCE
    webbrowser.c
    page.c)

add_executable(webbrowser ${SOURCE} webbrowser.rc)
set_module_type(webbrowser win32gui UNICODE)
target_link_libraries(webbrowser dfbrengine)
add_importlibs(webbrowser winhttp shlwapi user32 gdi32 msvcrt kernel32)
add_cd_file(TARGET webbrowser DESTINATION reactos/system32 FOR all)
