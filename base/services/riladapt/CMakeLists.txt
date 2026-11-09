
remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x600 -DWINVER=0x600)

list(APPEND SOURCE
    config.c
    host.c
    main.c
    riladapt.h)

add_executable(riladapt ${SOURCE} riladapt.rc)
target_link_libraries(riladapt uuid)
set_module_type(riladapt win32cui UNICODE)
add_importlibs(riladapt setupapi advapi32 advapi32_vista kernel32_vista msvcrt kernel32 ntdll)
add_pch(riladapt riladapt.h SOURCE)
add_cd_file(TARGET riladapt DESTINATION reactos/system32 FOR all)
