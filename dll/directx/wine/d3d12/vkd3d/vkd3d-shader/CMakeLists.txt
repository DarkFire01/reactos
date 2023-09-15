
include_directories("../include")
include_directories("../include/private")

FLEX_TARGET(preproc_scanner preproc.l ${CMAKE_CURRENT_BINARY_DIR}/preproc.yy.c)
BISON_TARGET(preproc_parser preproc.y ${CMAKE_CURRENT_BINARY_DIR}/preproc.tab.c COMPILE_FLAGS "-p asmshader_")
ADD_FLEX_BISON_DEPENDENCY(preproc_scanner preproc_parser)

FLEX_TARGET(hlsl_scanner hlsl.l ${CMAKE_CURRENT_BINARY_DIR}/hlsl.yy.c)
BISON_TARGET(hlsl_parser hlsl.y ${CMAKE_CURRENT_BINARY_DIR}/hlsl.tab.c COMPILE_FLAGS "-p hlsl_")
ADD_FLEX_BISON_DEPENDENCY(hlsl_scanner hlsl_parser)

list(APPEND MISC_ITEMS
    ${FLEX_preproc_scanner_OUTPUTS}
    ${BISON_preproc_parser_OUTPUTS}
    ${FLEX_hlsl_scanner_OUTPUTS}
    ${BISON_hlsl_parser_OUTPUTS})

add_library(vkd3d-shader checksum.c
                         d3dbc.c
                         dxbc.c
                         glsl.c
                         hlsl_codegen.c
                         hlsl_constant_ops.c
                         hlsl_sm1.c
                         hlsl_sm4.c
                         hlsl.c
                         spirv.c
                         trace.c
                         vkd3d_shader_main.c
                         ${MISC_ITEMS})


add_dependencies(vkd3d-shader vkd3d_idl_headers psdk)
