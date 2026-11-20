remove_definitions(-D_WIN32_WINNT=0x502)

add_definitions(
    -D_WIN32_WINNT=0x602
    -DNTDDI_VERSION=0x06020000)

spec2def(qcomapr.sys qcomapr.spec ADD_IMPORTLIB)

add_library(qcomapr MODULE
    apr.c
    qcomapr.rc
    ${CMAKE_CURRENT_BINARY_DIR}/qcomapr.def)

set_module_type(qcomapr kmdfdriver)
target_link_libraries(qcomapr ntoskrnl_vista libcntpr ${PSEH_LIB})
add_importlibs(qcomapr wdfldr qcomdbg ntoskrnl hal)
add_cd_file(TARGET qcomapr DESTINATION reactos/system32/drivers FOR all)
add_registry_inf(qcomapr_reg.inf)
