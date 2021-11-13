
add_definitions(-D_NTHAL_ -D_BLDR_ -D_NTSYSTEM_)

include_directories(BEFORE include)
include_directories(${REACTOS_SOURCE_DIR}/ntoskrnl/include)
include_directories(${REACTOS_SOURCE_DIR}/sdk/lib/cmlib)
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/reactos/libs)
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/reactos/elf)

function(add_target _boardname)
    cmake_parse_arguments(_boarddata "" "" "SOURCES;COMPONENTS" ${ARGN})

    # Create the actual target
    foreach(_component ${_boarddata_COMPONENTS})
        list(APPEND _boarddata_SOURCES "$<TARGET_OBJECTS:lib_board_${_component}>")
    endforeach()
    add_library(${_boardname}
        ${_boarddata_SOURCES})
endfunction()

include(generic.cmake)

if(ARCH STREQUAL "arm")
    include(versatile.cmake)

    add_target(versatile COMPONENTS generic versatile)
elseif(ARCH STREQUAL "arm64")
    add_target(applesilicon COMPONENTS generic)
endif()
