
add_definitions(
    -D_NTHALDLL_
    -D_NTHAL_)

include_directories(
    include
    ${REACTOS_SOURCE_DIR}/ntoskrnl/include
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/libs/fast486)

function(add_hal _halname)
    cmake_parse_arguments(_haldata "" "" "SOURCES;COMPONENTS" ${ARGN})

    # Handle the spec file for the dll name
    spec2def(${_halname}.dll ../hal.spec ADD_IMPORTLIB)

    # Create the actual target
    foreach(_component ${_haldata_COMPONENTS})
        list(APPEND _haldata_SOURCES "$<TARGET_OBJECTS:lib_hal_${_component}>")
    endforeach()
    add_library(${_halname} MODULE
        ${_haldata_SOURCES}
        ${CMAKE_CURRENT_BINARY_DIR}/hal.def)

    if(${_halname} STREQUAL "hal")
        target_link_libraries(${_halname} libcntpr arbiter fast486)
    else()
        target_link_libraries(${_halname} libcntpr fast486)
    endif()

    add_importlibs(${_halname} ntoskrnl)
    #add_pch(${_halname} include/hal.h)
    add_dependencies(${_halname} psdk asm)
    set_module_type(${_halname} kerneldll ENTRYPOINT HalInitSystem 8)
    add_cd_file(TARGET ${_halname} DESTINATION reactos/system32 NO_CAB FOR all)
    if(MSVC)
        target_link_options(${_halname} PRIVATE /ignore:4216 /SECTION:INIT,ERWD)
    else()
        target_link_libraries(${_halname} -lgcc)
    endif()
endfunction()

# The components
include(halmacpi.cmake)
include(halaacpi.cmake)

if(ARCH STREQUAL "i386")
    add_hal(halaacpi SOURCES apic/halaacpi.rc COMPONENTS generic acpi up apic)
    add_hal(halmacpi SOURCES smp/halmacpi.rc COMPONENTS halmacpi)
elseif(ARCH STREQUAL "amd64")
    add_hal(hal SOURCES ${HAL_SOURCE} COMPONENTS halaacpi)
    add_hal(halmp SOURCES ${HAL_SOURCE} smp/halmacpi.rc COMPONENTS halmacpi)
endif()
