list(APPEND SOURCE
    webcamviewer.c)

add_executable(webcamviewer ${SOURCE} rsrc.rc)
set_module_type(webcamviewer win32gui UNICODE)
add_importlibs(webcamviewer libjpeg ksuser user32 gdi32 comctl32 setupapi advapi32 kernel32 msvcrt)
add_cd_file(TARGET webcamviewer DESTINATION reactos FOR all)
add_cd_file(TARGET webcamviewer DESTINATION reactos/system32 FOR all)
