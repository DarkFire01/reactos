
add_definitions(-DUNICODE -D_UNICODE)

list(APPEND SOURCE
    caps.c
    report.c
    trnslate.c
    usages.c
    values.c
    hidparser.h)

# hid.dll only needs the report and capability API
add_library(hidparser_um STATIC ${SOURCE})
add_importlibs(hidparser_um ntdll)
add_dependencies(hidparser_um bugcodes xdk)
target_include_directories(hidparser_um INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

add_library(hidparser_km STATIC ${SOURCE} descript.c)
add_importlibs(hidparser_km ntoskrnl)
add_dependencies(hidparser_km bugcodes xdk)
target_include_directories(hidparser_km INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
