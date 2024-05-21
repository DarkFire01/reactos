include_directories(../../include)
include_directories(../../include/private)
include_directories(../../include/vulkan)
add_library(vkd3d        command.c
                         device.c
                         resource.c
                         state.c
                         utils.c
                         vkd3d_main.c)
add_dependencies(vkd3d psdk d3d_idl_headers)
