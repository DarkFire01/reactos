
remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x600 -DWINVER=0x600 -D_DIFXAPI_)

spec2def(difxapi.dll difxapi.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    difxapi.c
    log.c
    pnp.c
    store.c
    precomp.h)

add_library(difxapi MODULE
    ${SOURCE}
    difxapi.rc
    ${CMAKE_CURRENT_BINARY_DIR}/difxapi.def)

set_module_type(difxapi win32dll UNICODE)
target_link_libraries(difxapi wine)
add_importlibs(difxapi setupapi advapi32 user32 msvcrt kernel32 ntdll)
add_pch(difxapi precomp.h SOURCE)
add_cd_file(TARGET difxapi DESTINATION reactos/system32 FOR all)
