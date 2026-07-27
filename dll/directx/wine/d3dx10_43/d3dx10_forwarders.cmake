# Shared build definition for d3dx10_33 .. d3dx10_42.
#
# Unlike the d3dx9 and d3dcompiler families these are not rebuilt from the
# newest sources: each one is a forwarder DLL whose .spec sends ~172 entry
# points straight on to d3dx10_43, and whose only real code is its own
# D3DX10CheckVersion (which has to report its own SDK version, hence one small
# per-version source file rather than PARENTSRC).

function(add_d3dx10_forwarder VERSION)
    set(_target d3dx10_${VERSION})

    spec2def(${_target}.dll ${_target}.spec ADD_IMPORTLIB)

    add_library(${_target} MODULE
        ${_target}_main.c
        version.rc
        ${CMAKE_CURRENT_BINARY_DIR}/${_target}_stubs.c
        ${CMAKE_CURRENT_BINARY_DIR}/${_target}.def)

    target_compile_definitions(${_target} PRIVATE
        __WINESRC__
        __ROS_LONG64__)

    target_include_directories(${_target} BEFORE PRIVATE
        ${REACTOS_SOURCE_DIR}/sdk/include/wine
        ${REACTOS_BINARY_DIR}/sdk/include/wine
        ${CMAKE_CURRENT_BINARY_DIR})

    set_module_type(${_target} win32dll)
    target_link_libraries(${_target} wine)

    # None of these is an upstream IMPORTS entry. They are needed because MSVC
    # resolves a cross-DLL forwarder against the target's import library at
    # link time (the same reason cfgmgr32 has to list setupapi); GNU ld takes
    # them straight from the .def and does not care.
    #
    # Most entries forward to d3dx10_43, but a handful that only ever existed
    # in the older SDKs -- D3DX10DisassembleEffect, D3DX10DisassembleShader,
    # D3DX10ReflectShader, D3DX10GetDriverLevel -- forward to d3dx10_39 and
    # d3dx10_37, which is where upstream stubs them. Read the targets out of
    # the .spec rather than hardcoding them, so a resync that redirects a
    # forwarder does not quietly break the MSVC build.
    file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/${_target}.spec _spec_forwards
         REGEX "d3dx10_[0-9]+\\.")
    set(_forward_targets "")
    foreach(_line IN LISTS _spec_forwards)
        string(REGEX MATCH "d3dx10_[0-9]+" _forward "${_line}")
        if(_forward AND NOT _forward STREQUAL ${_target})
            list(APPEND _forward_targets ${_forward})
        endif()
    endforeach()
    if(_forward_targets)
        list(REMOVE_DUPLICATES _forward_targets)
    endif()

    add_importlibs(${_target} ${_forward_targets} msvcrt kernel32 ntdll)
    add_dependencies(${_target} wineheaders d3d_idl_headers)
    add_cd_file(TARGET ${_target} DESTINATION reactos/system32 FOR all)
endfunction()
