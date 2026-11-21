
# DIAG: prints the failure reasons the remote processors leave in shared memory
add_library(qcomsfr MODULE qcomsfr.c)

set_module_type(qcomsfr kernelmodedriver)
target_link_libraries(qcomsfr libcntpr)
add_importlibs(qcomsfr ntoskrnl hal)
add_cd_file(TARGET qcomsfr DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_registry_inf(qcomsfr_reg.inf)
