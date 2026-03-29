
remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x600 -DWINVER=0x600)

add_definitions(-D_KERNEL32_)
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/reactos/subsys ..)
include_directories(vista)
list(APPEND SOURCE
    vista/firmware.c
    vista/EnumSystemLocalesEx.c
    vista/GetLocaleInfoEx.c
    vista/GetFileInformationByHandleEx.c
    vista/GetFinalPathNameByHandle.c
    vista/GetNamedPipeClientProcessId.c
    vista/GetTickCount64.c
    vista/GetUserDefaultLocaleName.c
    vista/InitOnce.c
    vista/IsValidLocaleName.c
    vista/LCIDToLocaleName.c
    vista/LocaleNameToLCID.c
    vista/SetThreadDescription.c
    vista/sync.c
    vista/threadpool.c
    vista/vista.c)

add_library(kernel32_vista_static ${SOURCE})
target_link_libraries(kernel32_vista_static ${PSEH_LIB})
add_dependencies(kernel32_vista_static psdk)
