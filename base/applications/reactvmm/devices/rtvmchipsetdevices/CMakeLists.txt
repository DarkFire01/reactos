spec2def(rtvmchipsetdevices.dll rtvmchipsetdevices.spec)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../common)

add_library(rtvmchipsetdevices MODULE
    module.cpp
    ../common/vdevbase.cpp
    ioapicdevice.cpp
    rtvmchipsetdevices.rc
    ${CMAKE_CURRENT_BINARY_DIR}/rtvmchipsetdevices.def)
set_module_type(rtvmchipsetdevices win32dll ENTRYPOINT DllMain 12)
target_link_libraries(rtvmchipsetdevices cpprt)
add_importlibs(rtvmchipsetdevices ole32 msvcrt kernel32 ntdll)
add_dependencies(rtvmchipsetdevices psdk)
add_cd_file(TARGET rtvmchipsetdevices DESTINATION reactos/system32 FOR all)
