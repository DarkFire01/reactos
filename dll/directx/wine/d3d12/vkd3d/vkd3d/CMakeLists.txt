
include_directories("../include")
include_directories("../include/private")
add_library(vkd3d command.c
                  device.c
                  resource.c
                  state.c
                  utils.c
                  vkd3d_main.c)


add_dependencies(vkd3d vkd3d_idl_headers psdk)
