spec2def(rtvmemulateddevices.dll rtvmemulateddevices.spec)

add_library(rtvmemulateddevices MODULE
    module.cpp
    vdevbase.cpp
    picdevice.cpp
    rtvmemulateddevices.rc
    ${CMAKE_CURRENT_BINARY_DIR}/rtvmemulateddevices.def)
set_module_type(rtvmemulateddevices win32dll ENTRYPOINT DllMain 12)
target_link_libraries(rtvmemulateddevices cpprt)
add_importlibs(rtvmemulateddevices ole32 msvcrt kernel32 ntdll)
add_dependencies(rtvmemulateddevices psdk)
add_cd_file(TARGET rtvmemulateddevices DESTINATION reactos/system32 FOR all)
