remove_definitions(-D_WIN32_WINNT=0x502)

add_definitions(
    -D_WIN32_WINNT=0x602
    -DNTDDI_VERSION=0x06020000)

spec2def(qcomq6.sys qcomq6.spec ADD_IMPORTLIB)

add_library(qcomq6 MODULE
    q6.c
    q6graph.c
    q6voice.c
    q6smmu.c
    qcomq6.rc
    ${CMAKE_CURRENT_BINARY_DIR}/qcomq6.def)

set_module_type(qcomq6 kmdfdriver)
target_link_libraries(qcomq6 ntoskrnl_vista libcntpr ${PSEH_LIB})
add_importlibs(qcomq6 qcomapr qcomamp wdfldr qcomdbg ntoskrnl hal)
add_cd_file(TARGET qcomq6 DESTINATION reactos/system32/drivers FOR all)
add_registry_inf(qcomq6_reg.inf)
