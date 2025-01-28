add_asm_files(wow64_asm srasm.S)
add_library(wow64 MODULE wow64.c registry.c struct32.h file.c system.c syscall.c sync.c virtual.c ${wow64_asm})

set_module_type(wow64 win32dll UNICODE ENTRYPOINT DllMain 12)
#############################################
## HACK FOR MSVC COMPILATION WITH win32dll ##
set_subsystem(wow64 console)
################# END  HACK #################

# FIXME: force a base address below 4 GiB, this one was chosen arbitrarily
set_image_base(wow64 0xFF000000)
add_importlibs(wow64 ntdll)

add_cd_file(TARGET wow64 DESTINATION reactos/system32 NO_CAB FOR all)