spec2def(rstrtmgr.dll rstrtmgr.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    rstrtmgr.c
    ${CMAKE_CURRENT_BINARY_DIR}/rstrtmgr_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/rstrtmgr.def)

add_library(rstrtmgr MODULE ${SOURCE} rstrtmgr.rc)
set_module_type(rstrtmgr win32dll)
target_link_libraries(rstrtmgr wine)
add_importlibs(rstrtmgr msvcrt kernel32 ntdll)
add_cd_file(TARGET rstrtmgr DESTINATION reactos/system32 FOR all)
