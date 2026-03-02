add_definitions(-D_DLL -D__USE_CRTIMP)
include_directories(${REACTOS_SOURCE_DIR}/win32ss/include)
add_library(wow64win MODULE gdi.c syscall.c user_ros.c user.c)

set_module_type(wow64win win32dll UNICODE ENTRYPOINT DllMain 12)
#############################################
## HACK FOR MSVC COMPILATION WITH win32dll ##
set_subsystem(wow64win console)
################# END  HACK #################

target_link_libraries(wow64win win32ksys)
add_importlibs(wow64win ntdll wow64)
add_dependencies(wow64win xdk)
add_cd_file(TARGET wow64win DESTINATION reactos/system32 FOR all)
