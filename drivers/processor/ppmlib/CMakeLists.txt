
remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(-D_WIN32_WINNT=0x603 -DNTDDI_VERSION=0x06030000)

list(APPEND SOURCE
    acpi.c
    cppc.c
    device.c
    gas.c
    idle.c
    init.c
    perf.c
    ppmlib.h)

add_library(ppmlib STATIC ${SOURCE})
add_dependencies(ppmlib bugcodes xdk)
target_link_libraries(ppmlib wdmguid)
if(ARCH STREQUAL "i386")
    # The x86 kernel does not export memcmp
    target_link_libraries(ppmlib memcmp)
endif()
target_include_directories(ppmlib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
