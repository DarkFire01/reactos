
# NET_BUFFER_LIST and the NDIS 6 offload information only exist from Vista
# onwards, so the tree default has to come off before this library's floor
# goes on.
remove_definitions(-D_WIN32_WINNT=0x502)

add_definitions(
    -D_WIN32_WINNT=0x603
    -DNTDDI_VERSION=0x06030000
    -DNDIS_SUPPORT_NDIS680=1
    -DNDIS60=1)

list(APPEND SOURCE
    rsclib.cpp)

add_library(rsclib STATIC ${SOURCE})
add_dependencies(rsclib bugcodes xdk)
target_include_directories(rsclib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
