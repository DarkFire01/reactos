
spec2def(ksecdd.sys ksecdd.spec ADD_IMPORTLIB)

include_directories(
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/drivers/ksecdd)

list(APPEND SOURCE
    ksecdd.c
    dispatch.c
    crypt.c
    random.c
    stubs.c
    ksecdd.rc
    ${CMAKE_CURRENT_BINARY_DIR}/ksecdd.def)

add_library(ksecdd MODULE ${SOURCE})
target_link_libraries(ksecdd cryptlib pseh)
set_module_type(ksecdd kernelmodedriver)
add_importlibs(ksecdd cng ntoskrnl hal)
if(ARCH STREQUAL "i386")
    # The x86 kernel does not export memcmp
    target_link_libraries(ksecdd memcmp)
endif()
add_cd_file(TARGET ksecdd DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_registry_inf(ksecdd_reg.inf)
