
# Wine's bundled vkd3d, built as a single static library (matching upstream's
# EXTLIB = libvkd3d.a). Consumers add ${VKD3D_INCLUDE_DIRS} and link "vkd3d".

set(VKD3D_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/private
    CACHE INTERNAL "vkd3d public and private include directories")

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/private
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/vkd3d
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/vkd3d-shader
    ${CMAKE_CURRENT_BINARY_DIR})

# vkd3d_common.h uses CONDITION_VARIABLE/SRWLOCK, which the PSDK gates behind
# _WIN32_WINNT >= 0x600. The backing exports exist in kernel32/ntdll (and are
# present in the import libs as long as DLL_EXPORT_VERSION >= 0x600).
# Spelled 0x0600 to match vkd3d-common/debug.c's own #define token-for-token,
# so its redefinition is benign rather than a -Werror redefinition warning.
remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(
    -D_WIN32_WINNT=0x0600
    -DWINVER=0x0600
    -D_USE_MATH_DEFINES
    # The flex scanners reference stdin/stdout/stderr. <stdio.h> defines those
    # in terms of the _iob array, which msvcrt only exposes via __iob_func(),
    # leaving an unresolved data import. Take the header's own
    # _STDSTREAM_DEFINED override hook and route them through the function.
    -D_STDSTREAM_DEFINED
    "-Dstdin=(&__iob_func()[0])"
    "-Dstdout=(&__iob_func()[1])"
    "-Dstderr=(&__iob_func()[2])"
    -DWINE_NO_NAMELESS_EXTENSION
    -DLIBVKD3D_SOURCE
    -DLIBVKD3D_SHADER_SOURCE
    -DLIBVKD3D_UTILS_SOURCE
    -DHAVE_CONFIG_H)

list(APPEND SOURCE
    libs/vkd3d-common/blob.c
    libs/vkd3d-common/debug.c
    libs/vkd3d-common/error.c
    libs/vkd3d-common/memory.c
    libs/vkd3d-common/utf8.c
    libs/vkd3d-shader/checksum.c
    libs/vkd3d-shader/d3d_asm.c
    libs/vkd3d-shader/d3dbc.c
    libs/vkd3d-shader/dxbc.c
    libs/vkd3d-shader/dxil.c
    libs/vkd3d-shader/fx.c
    libs/vkd3d-shader/glsl.c
    libs/vkd3d-shader/hlsl.c
    libs/vkd3d-shader/hlsl_codegen.c
    libs/vkd3d-shader/hlsl_constant_ops.c
    libs/vkd3d-shader/ir.c
    libs/vkd3d-shader/msl.c
    libs/vkd3d-shader/spirv.c
    libs/vkd3d-shader/tpf.c
    libs/vkd3d-shader/vkd3d_shader_main.c
    libs/vkd3d/cache.c
    libs/vkd3d/command.c
    libs/vkd3d/device.c
    libs/vkd3d/resource.c
    libs/vkd3d/state.c
    libs/vkd3d/utils.c
    libs/vkd3d/vkd3d_main.c)

# The scanners/parsers carry their own api.prefix (hlsl_yy, preproc_yy) in the
# .l/.y sources, so no -p flag is needed here.
FLEX_TARGET(vkd3d_hlsl_scanner libs/vkd3d-shader/hlsl.l ${CMAKE_CURRENT_BINARY_DIR}/hlsl.yy.c)
BISON_TARGET(vkd3d_hlsl_parser libs/vkd3d-shader/hlsl.y ${CMAKE_CURRENT_BINARY_DIR}/hlsl.tab.c
             DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/hlsl.tab.h)
ADD_FLEX_BISON_DEPENDENCY(vkd3d_hlsl_scanner vkd3d_hlsl_parser)

FLEX_TARGET(vkd3d_preproc_scanner libs/vkd3d-shader/preproc.l ${CMAKE_CURRENT_BINARY_DIR}/preproc.yy.c)
BISON_TARGET(vkd3d_preproc_parser libs/vkd3d-shader/preproc.y ${CMAKE_CURRENT_BINARY_DIR}/preproc.tab.c
             DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/preproc.tab.h)
ADD_FLEX_BISON_DEPENDENCY(vkd3d_preproc_scanner vkd3d_preproc_parser)

add_library(vkd3d
    ${SOURCE}
    ${FLEX_vkd3d_hlsl_scanner_OUTPUTS}
    ${BISON_vkd3d_hlsl_parser_OUTPUTS}
    ${FLEX_vkd3d_preproc_scanner_OUTPUTS}
    ${BISON_vkd3d_preproc_parser_OUTPUTS})

# Generated scanner/parser sources need the vkd3d-shader source dir on the
# include path to find hlsl.h / preproc.h.
target_include_directories(vkd3d PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/libs/vkd3d-shader)

if(MSVC)
    # Suppressions below are all for upstream vkd3d code that is correct as
    # written, or benign at the scales this code operates on. Deliberately NOT
    # suppressed: C4700 (uninitialised variable) and C4715 (not all paths
    # return a value), which are real defect detectors.
    #
    # C4090: differing 'const' qualifiers, widespread in vkd3d.
    # C4200: zero-sized array in struct -- deliberate C99 flexible array
    #        members; must not be "fixed" to a fixed-size array.
    # C4098: 'void' function returning a void expression -- valid C99, and a
    #        common Wine idiom.
    # C4146: unary minus on unsigned, intentional in the bit-twiddling paths.
    # C4245/C4305/C4309: signed/unsigned and float literal conversions in the
    #        shader constant tables.
    # C4267/C4319: align() masks with a size_t-wide value and the SM1 encoder
    #        narrows a token count to 16 bits. Both are upstream and hold for
    #        any real shader (offsets stay far below 4 GB, SM1 counts are
    #        16-bit by format), but they are worth revisiting upstream.
    target_compile_options(vkd3d PRIVATE
        /wd4090 /wd4098 /wd4146 /wd4200 /wd4245 /wd4267 /wd4305 /wd4309 /wd4319)
else()
    target_compile_options(vkd3d PRIVATE -Wno-unused-but-set-variable)
endif()

# d3d12shader.h (via vkd3d_shader.h) pulls in the widl-generated d3dcommon.h,
# which lives in the d3d_idl_headers target rather than psdk.
# preproc.y uses POSIX stat()/fstat()/fileno()/S_IFREG. oldnames provides those
# aliases and, via INTERFACE definitions, the _CRT_DECLARE_NONSTDC_NAMES and
# _CRT_NONSTDC_NO_DEPRECATE pair that makes them visible under MSVC.
target_link_libraries(vkd3d oldnames)

add_dependencies(vkd3d psdk d3d_idl_headers)
