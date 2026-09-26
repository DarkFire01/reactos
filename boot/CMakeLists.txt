
include(boot_images.cmake)

add_subdirectory(bootdata)

# ReactV lives next to the source root and links into the boot loader
if(EXISTS "${REACTOS_SOURCE_DIR}/../ReactV/CMakeLists.txt")
    add_subdirectory("${REACTOS_SOURCE_DIR}/../ReactV" reactv)
endif()

add_subdirectory(freeldr)
add_subdirectory(rtl)

add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/remaster.cmd DESTINATION loader NO_CAB FOR bootcd)
add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/remaster.sh DESTINATION loader NO_CAB FOR bootcd)
