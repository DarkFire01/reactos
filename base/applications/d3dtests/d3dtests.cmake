
# Shared build definition for the d3dtest_* programs.
#
# Every test is one .c file plus whatever import libraries it needs, so rather
# than carry ~50 near-identical CMakeLists we declare them all from one list.
#
#   add_d3dtest(<name> <source> [import libs...])
#
# Tests are console programs installed to reactos/system32 so they can be run
# straight from a command prompt on the live system.

function(add_d3dtest _name _source)
    add_executable(${_name} ${_source})
    set_module_type(${_name} win32cui)
    target_include_directories(${_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # dxguid carries the pre-D3D10 GUIDs, dx10guid the IDXGI*/ID3D10*/ID3D11*
    # ones; tests reference both families.
    target_link_libraries(${_name} dxguid dx10guid uuid)
    add_importlibs(${_name} ${ARGN} user32 gdi32 ole32 msvcrt kernel32)
    add_cd_file(TARGET ${_name} DESTINATION reactos/system32 FOR all)
endfunction()
