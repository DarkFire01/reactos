add_definitions(-D_DLL)
spec2def(wow64cpu.dll def/wow64cpu.spec ADD_IMPORTLIB)
add_library(wow64cpu MODULE 
    cpu.c
    cpu.h
    ${CMAKE_CURRENT_BINARY_DIR}/wow64cpu.def
    ${CMAKE_CURRENT_BINARY_DIR}/wow64cpu_stubs.c)

set_module_type(wow64cpu win32dll UNICODE ENTRYPOINT DllMain 12)
#############################################
## HACK FOR MSVC COMPILATION WITH win32dll ##
set_subsystem(wow64cpu console)
################# END  HACK #################

target_link_libraries(wow64cpu libwow64common)
add_importlibs(wow64cpu ntdll)
add_dependencies(wow64cpu xdk psdk)
add_cd_file(TARGET wow64cpu DESTINATION reactos/system32 NO_WOW64 FOR all)
