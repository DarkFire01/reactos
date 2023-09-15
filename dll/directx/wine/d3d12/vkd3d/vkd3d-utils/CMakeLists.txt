
include_directories("../include")
include_directories("../include/private")
add_library(vkd3d-utils vkd3d_utils_main.c)


add_dependencies(vkd3d-utils vkd3d_idl_headers psdk)
