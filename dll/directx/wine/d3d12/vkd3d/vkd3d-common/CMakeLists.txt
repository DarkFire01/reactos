include_directories("../include")
include_directories("../include/private")
add_library(vkd3d-common blob.c
                         debug.c
                         error.c
                         memory.c
                         utf8.c)

add_dependencies(vkd3d-common vkd3d_idl_headers psdk)
