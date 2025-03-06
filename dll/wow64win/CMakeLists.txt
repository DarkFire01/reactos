add_definitions(-D_DLL -D__USE_CRTIMP)
include_directories(${REACTOS_SOURCE_DIR}/win32ss/include)
add_library(wow64win MODULE gdi.c)

set_module_type(wow64win win32dll UNICODE ENTRYPOINT DllMain 12)
#############################################
## HACK FOR MSVC COMPILATION WITH win32dll ##
set_subsystem(wow64win console)
################# END  HACK #################

add_importlibs(wow64win ntdll win32u)
add_delay_importlibs(wow64win win32u)
add_dependencies(wow64win xdk)
add_cd_file(TARGET wow64win DESTINATION reactos/system32 NO_CAB FOR all)