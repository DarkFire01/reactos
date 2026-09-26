
include_directories(${REACTOS_SOURCE_DIR}/sdk/lib/3rdparty/symcrypt/inc)

list(APPEND SOURCE
    cipher.c
    hash.c
    provider.c)

add_library(bcryptk ${SOURCE})
if(MSVC)
    # warning C4068: unknown pragma 'GCC', from the SymCrypt headers
    target_compile_options(bcryptk PRIVATE /wd4068)
endif()

add_dependencies(bcryptk xdk)
target_include_directories(bcryptk INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
