include_directories(../../include)
include_directories(../../include/private)
include_directories(../../include/vulkan)


#FLEX_TARGET(hlsl_scanner hlsl.l ${CMAKE_CURRENT_BINARY_DIR}/hlsl.yy.c)
#BISON_TARGET(hlsl_parser hlsl.y ${CMAKE_CURRENT_BINARY_DIR}/hlsl.tab.c COMPILE_FLAGS "-p hlsl_")
#ADD_FLEX_BISON_DEPENDENCY(hlsl_scanner hlsl_parser)
#
#FLEX_TARGET(preproc_scanner preproc.l ${CMAKE_CURRENT_BINARY_DIR}/preproc.yy.c)
#BISON_TARGET(preproc_parser preproc.y ${CMAKE_CURRENT_BINARY_DIR}/preproc.tab.c COMPILE_FLAGS "-p preproc_")
#ADD_FLEX_BISON_DEPENDENCY(preproc_scanner preproc_parser)
#
#list(APPEND PCH_SKIP_SOURCE
#    ${FLEX_hlsl_scanner_OUTPUTS})
#    #${BISON_hlsl_parser_OUTPUTS}
#    #${FLEX_preproc_scanner_OUTPUTS}
#    #${BISON_preproc_parser_OUTPUTS})

list(APPEND VKD3DSHADER_SOURCE
     checksum.c
     d3d_asm.c
     d3dbc.c
     dxbc.c
     dxil.c
     fx.c
     glsl.c
     hlsl_codegen.c
     hlsl_constant_ops.c
     hlsl.c
     ir.c
     spirv.c
     tpf.c
     vkd3d_shader_main.c)

add_library(vkd3d-shader 
                         ${VKD3DSHADER_SOURCE})
target_link_libraries(vkd3d-shader wine)                   
add_dependencies(vkd3d-shader psdk d3d_idl_headers)

#add_pch(vkd3d-shader vkd3d_shader_private.h "${PCH_SKIP_SOURCE}")