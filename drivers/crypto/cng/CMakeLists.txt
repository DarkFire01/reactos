
spec2def(cng.sys cng.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    asym.c
    cng.c
    entropy.c
    hash.c
    kdf.c
    key.c
    provider.c
    scenv.c
    ${CMAKE_CURRENT_BINARY_DIR}/cng.def)

add_library(cng MODULE ${SOURCE} cng.rc)
target_link_libraries(cng symcrypt)
set_module_type(cng kernelmodedriver)
add_importlibs(cng ntoskrnl hal)
if(ARCH STREQUAL "i386")
    # The x86 kernel does not export memcmp
    target_link_libraries(cng memcmp)
endif()
add_cd_file(TARGET cng DESTINATION reactos/system32/drivers NO_CAB FOR all)
