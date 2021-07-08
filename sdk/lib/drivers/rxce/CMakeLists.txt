remove_definitions(-D_WIN32_WINNT=0x600 -DWINVER=0x600)
add_definitions(-D_WIN32_WINNT=0x502)
add_definitions(-DUNICODE -D_UNICODE -DRDBSS_TRACKER)

include_directories(${REACTOS_SOURCE_DIR}/drivers/filesystems/mup)

list(APPEND SOURCE
    rxce.c)

add_library(rxce ${SOURCE})
target_link_libraries(rxce PRIVATE pseh)
add_dependencies(rxce bugcodes xdk)
