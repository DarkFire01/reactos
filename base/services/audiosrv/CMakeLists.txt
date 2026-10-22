remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(-D_WIN32_WINNT=0x600)

list(APPEND ENGINE_SOURCE
    engine/devices.cpp
    engine/endpoints.cpp
    engine/format.cpp
    engine/server.cpp
    engine/wavedrv.cpp
    engine/engine.h)

add_library(audioengine STATIC ${ENGINE_SOURCE})
target_compile_definitions(audioengine PRIVATE UNICODE _UNICODE)
target_link_libraries(audioengine cppstl)
add_dependencies(audioengine psdk)

list(APPEND SOURCE
    main.c
    pnp_list_manager.c
    pnp_list_lock.c
    pnp.c
    services.c
    audiosrv.h)

add_executable(audiosrv ${SOURCE} audiosrv.rc)
set_module_type(audiosrv win32cui UNICODE)
target_link_libraries(audiosrv audioengine cppstl uuid)
add_importlibs(audiosrv advapi32 user32 setupapi winmm ole32 msvcrt kernel32 ntdll)
add_pch(audiosrv audiosrv.h SOURCE)
add_cd_file(TARGET audiosrv DESTINATION reactos/system32 FOR all)
