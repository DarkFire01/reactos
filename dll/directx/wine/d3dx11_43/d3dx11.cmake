# Shared build definition for the d3dx11_NN family.
#
# Upstream builds d3dx11_42 from dlls/d3dx11_43 via PARENTSRC, varying only
# D3DX11_SDK_VERSION; the sibling directory holds nothing but a .spec and a
# version.rc. Keep the source list in sync with dlls/d3dx11_43/Makefile.in on
# every resync.

function(add_d3dx11_target VERSION)
    set(_target d3dx11_${VERSION})
    set(_srcdir ${CMAKE_CURRENT_SOURCE_DIR}/../d3dx11_43)

    spec2def(${_target}.dll ${_target}.spec ADD_IMPORTLIB)

    set(_source
        ${_srcdir}/async.c
        ${_srcdir}/main.c
        ${_srcdir}/texture.c)

    add_library(${_target} MODULE
        ${_source}
        version.rc
        ${CMAKE_CURRENT_BINARY_DIR}/${_target}_stubs.c
        ${CMAKE_CURRENT_BINARY_DIR}/${_target}.def)

    target_compile_definitions(${_target} PRIVATE
        __WINESRC__
        __ROS_LONG64__
        D3DX11_SDK_VERSION=${VERSION})

    target_include_directories(${_target} BEFORE PRIVATE
        ${REACTOS_SOURCE_DIR}/sdk/include/wine
        ${REACTOS_BINARY_DIR}/sdk/include/wine
        ${_srcdir}
        ${CMAKE_CURRENT_BINARY_DIR})

    set_module_type(${_target} win32dll)
    target_link_libraries(${_target} wine dxguid uuid)
    # Upstream's sole import is "d3dcompiler", which is d3dcompiler_47's import
    # library; every version keeps its full name here, so link that one.
    add_importlibs(${_target} d3dcompiler_47 msvcrt kernel32 ntdll)
    add_dependencies(${_target} wineheaders d3d_idl_headers)
    add_cd_file(TARGET ${_target} DESTINATION reactos/system32 FOR all)
endfunction()
