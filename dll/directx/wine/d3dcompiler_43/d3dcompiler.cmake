
# Shared build definition for the whole d3dcompiler_NN family.
#
# Upstream builds every version from dlls/d3dcompiler_43 via PARENTSRC, varying
# only D3D_COMPILER_VERSION; the sibling directories hold nothing but a .spec
# and a version.rc. Keep the source list in sync with
# dlls/d3dcompiler_43/Makefile.in on every resync.
#
# Upstream names d3dcompiler_47's import library "d3dcompiler" (no suffix);
# here every version keeps its full name, so dependants link d3dcompiler_47.

function(add_d3dcompiler_target VERSION)
    set(_target d3dcompiler_${VERSION})
    set(_srcdir ${CMAKE_CURRENT_SOURCE_DIR}/../d3dcompiler_43)

    spec2def(${_target}.dll ${_target}.spec ADD_IMPORTLIB)

    set(_source
        ${_srcdir}/asmparser.c
        ${_srcdir}/blob.c
        ${_srcdir}/bytecodewriter.c
        ${_srcdir}/compiler.c
        ${_srcdir}/reflection.c
        ${_srcdir}/utils.c)

    # asmshader.l/.y declare their own "asmshader_" prefix (%option prefix and
    # %define api.prefix). Passing -p as well makes bison fail outright.
    FLEX_TARGET(${_target}_asmshader_scanner ${_srcdir}/asmshader.l
                ${CMAKE_CURRENT_BINARY_DIR}/asmshader.yy.c)
    BISON_TARGET(${_target}_asmshader_parser ${_srcdir}/asmshader.y
                 ${CMAKE_CURRENT_BINARY_DIR}/asmshader.tab.c
                 DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/asmshader.tab.h)
    ADD_FLEX_BISON_DEPENDENCY(${_target}_asmshader_scanner ${_target}_asmshader_parser)

    add_library(${_target} MODULE
        ${_source}
        ${FLEX_${_target}_asmshader_scanner_OUTPUTS}
        ${BISON_${_target}_asmshader_parser_OUTPUTS}
        version.rc
        ${CMAKE_CURRENT_BINARY_DIR}/${_target}_stubs.c
        ${CMAKE_CURRENT_BINARY_DIR}/${_target}.def)

    target_compile_definitions(${_target} PRIVATE
        __WINESRC__
        __ROS_LONG64__
        D3D_COMPILER_VERSION=${VERSION})

    target_include_directories(${_target} BEFORE PRIVATE
        ${REACTOS_SOURCE_DIR}/sdk/include/wine
        ${REACTOS_BINARY_DIR}/sdk/include/wine
        ${_srcdir}
        ${CMAKE_CURRENT_BINARY_DIR}
        ${VKD3D_INCLUDE_DIRS})

    set_module_type(${_target} win32dll)
    # Do NOT link libvkd3d here: upstream's only import is wined3d, which
    # re-exports the vkd3d-shader API (40 entries in wined3d.spec). Linking the
    # static library as well would drag in vkd3d's D3D12 runtime and its IIDs.
    # oldnames supplies the POSIX aliases used here (strdup, stricmp) plus the
    # CRT opt-ins that declare them under MSVC.
    target_link_libraries(${_target} wine dxguid uuid oldnames)
    add_importlibs(${_target} d3dwine msvcrt kernel32 ntdll)
    add_dependencies(${_target} wineheaders d3d_idl_headers)
    add_cd_file(TARGET ${_target} DESTINATION reactos/system32 FOR all)
endfunction()
