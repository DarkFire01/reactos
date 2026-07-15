remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(-D_WIN32_WINNT=0xA00)

add_definitions(-DUNICODE -D_UNICODE -D_NTOSKRNL_ -D_NTSYSTEM_)

include_directories(${REACTOS_SOURCE_DIR}/sdk/lib/rtl)

list(APPEND SOURCE
    em.c
    etw.c
    ex.c
    fsrtl.c
    hvl.c
    io.c
    ke.c
    mm.c
    nt.c
    po.c
    pofx.c
    rtl.c
    string.c
    whea.c
    ${REACTOS_SOURCE_DIR}/sdk/lib/rtl/utf8.c)

add_library(ntoskrnl_vista ${SOURCE})
target_link_libraries(ntoskrnl_vista PRIVATE pseh) # rtl_vista)
add_dependencies(ntoskrnl_vista bugcodes xdk)

target_compile_definitions(ntoskrnl_vista PUBLIC NTKRNLVISTA)
