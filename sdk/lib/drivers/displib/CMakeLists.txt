remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(-D_WIN32_WINNT=0x602)
include_directories("../../../../win32ss_vista/include/")
add_definitions(-D_NTSYSTEM_)

list(APPEND SOURCE
     displib.c
     disp_win8.c
     displib_manage.c)

add_library(displib ${SOURCE})
add_dependencies(displib bugcodes xdk)
