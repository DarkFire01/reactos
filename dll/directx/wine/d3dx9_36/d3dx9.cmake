# Shared build definition for the whole d3dx9_NN family.
#
# Upstream builds every version from dlls/d3dx9_36 via PARENTSRC, varying only
# D3DX_SDK_VERSION; the sibling directories hold nothing but a .spec. Keep the
# source list in sync with dlls/d3dx9_36/Makefile.in on every resync.
#
# Upstream gives d3dx9_36 the import library name "d3dx9" and d3dx9_43 the name
# "d3dx9_43"; here every version keeps its full name, as with d3dcompiler.

function(add_d3dx9_target VERSION)
    set(_target d3dx9_${VERSION})
    set(_srcdir ${CMAKE_CURRENT_SOURCE_DIR}/../d3dx9_36)

    spec2def(${_target}.dll ${_target}.spec ADD_IMPORTLIB)

    set(_source
        ${_srcdir}/animation.c
        ${_srcdir}/core.c
        ${_srcdir}/effect.c
        ${_srcdir}/font.c
        ${_srcdir}/line.c
        ${_srcdir}/main.c
        ${_srcdir}/math.c
        ${_srcdir}/mesh.c
        ${_srcdir}/preshader.c
        ${_srcdir}/render.c
        ${_srcdir}/shader.c
        ${_srcdir}/skin.c
        ${_srcdir}/sprite.c
        ${_srcdir}/surface.c
        ${_srcdir}/texture.c
        ${_srcdir}/txc_compress_dxtn.c
        ${_srcdir}/txc_fetch_dxtn.c
        ${_srcdir}/util.c
        ${_srcdir}/volume.c
        ${_srcdir}/xfile.c)

    add_library(${_target} MODULE
        ${_source}
        version.rc
        ${CMAKE_CURRENT_BINARY_DIR}/${_target}_stubs.c
        ${CMAKE_CURRENT_BINARY_DIR}/${_target}.def)

    target_compile_definitions(${_target} PRIVATE
        __WINESRC__
        __ROS_LONG64__
        # math.c wants M_PI, which <math.h> only exposes on request here.
        _USE_MATH_DEFINES
        D3DX_SDK_VERSION=${VERSION})

    target_include_directories(${_target} BEFORE PRIVATE
        ${REACTOS_SOURCE_DIR}/sdk/include/wine
        ${REACTOS_BINARY_DIR}/sdk/include/wine
        ${_srcdir}
        ${CMAKE_CURRENT_BINARY_DIR})

    if(MSVC)
        # effect.c's SET_D3D_STATE forwards __VA_ARGS__ from one variadic macro
        # into another. MSVC's legacy preprocessor hands the whole pack to the
        # inner macro as a single argument, so it sees a method name and no
        # arguments at all ("element_count: not a function"). /Zc:preprocessor
        # selects the conformant preprocessor, which expands it the way C99 and
        # GCC do. Nothing here needs the legacy behaviour.
        target_compile_options(${_target} PRIVATE /Zc:preprocessor)
    endif()

    set_module_type(${_target} win32dll)
    # oldnames supplies the POSIX aliases used here (strdup, stricmp) plus the
    # CRT opt-ins that declare them under MSVC.
    target_link_libraries(${_target} wine dxguid uuid oldnames)
    # kernel32_vista for InitOnceExecuteOnce (shader.c): the plain kernel32
    # importlib is trimmed to DLL_EXPORT_VERSION and does not carry it.
    # Upstream's "d3dcompiler" import is d3dcompiler_47's; every version here
    # keeps its full name, so link that one.
    # usp10 is the one deliberate divergence from upstream's IMPORTS line.
    # font.c calls ScriptBreak, and Wine folded the ~40 Uniscribe entry points
    # into its own gdi32, so upstream needs no separate import. ReactOS keeps
    # them in usp10, the way Windows does, so name it explicitly.
    add_importlibs(${_target}
        d3d9 d3dcompiler_47 d3dxof ole32 gdi32 user32 usp10
        kernel32_vista msvcrt kernel32 ntdll)
    # surface.c reaches for WIC only when asked to load an image file, and
    # upstream delay-loads it for exactly that reason.
    add_delay_importlibs(${_target} windowscodecs)
    add_dependencies(${_target} wineheaders d3d_idl_headers)
    add_cd_file(TARGET ${_target} DESTINATION reactos/system32 FOR all)
endfunction()
