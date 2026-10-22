
remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(-D_WIN32_WINNT=0x600)

spec2def(mmdevapi.dll mmdevapi.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    activate.cpp
    client.cpp
    endpointvolume.cpp
    engine.cpp
    enumerator.cpp
    factory.cpp
    properties.cpp
    session.cpp
    precomp.h)

add_library(mmdevapi MODULE
    ${SOURCE}
    guid.c
    mmdevapi.rc
    ${CMAKE_CURRENT_BINARY_DIR}/mmdevapi_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/mmdevapi.def)

set_module_type(mmdevapi win32dll UNICODE)
target_link_libraries(mmdevapi uuid cppstl)
add_importlibs(mmdevapi ole32 oleaut32 advapi32 msvcrt kernel32 ntdll)
add_pch(mmdevapi precomp.h SOURCE)
set_source_files_properties(guid.c PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
add_dependencies(mmdevapi psdk)
add_cd_file(TARGET mmdevapi DESTINATION reactos/system32 FOR all)
