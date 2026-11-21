remove_definitions(-D_WIN32_WINNT=0x502)

add_definitions(
    -D_WIN32_WINNT=0xA00
    -DNTDDI_VERSION=0x0A000000)

add_library(qcomaudio MODULE
    adapter.cpp
    wave.cpp
    topo.cpp
    telephony.cpp
    guid.cpp
    qcomaudio.rc)

set_module_type(qcomaudio wdmdriver UNICODE)
target_link_libraries(qcomaudio stdunk libcntpr uuid)
add_importlibs(qcomaudio portcls qcomq6 qcomamp hal ntoskrnl)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(qcomaudio PRIVATE -Wno-write-strings)
endif()

add_pch(qcomaudio precomp.h "guid.cpp")
add_cd_file(TARGET qcomaudio DESTINATION reactos/system32/drivers FOR all)
add_registry_inf(qcomaudio_reg.inf)
