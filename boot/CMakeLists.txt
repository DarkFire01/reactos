
include(boot_images.cmake)

add_subdirectory(bootdata)

# React-V lives next to the source root and links into the boot loader, on the
# wide machine only: what it brings up is a hypervisor, and there is no narrow
# one to bring up.
if(ARCH STREQUAL "amd64" AND EXISTS "${REACTOS_SOURCE_DIR}/../React-V/CMakeLists.txt")
    add_subdirectory("${REACTOS_SOURCE_DIR}/../React-V" reactv)
endif()

add_subdirectory(freeldr)
add_subdirectory(rtl)

add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/remaster.cmd DESTINATION loader NO_CAB FOR bootcd)
add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/remaster.sh DESTINATION loader NO_CAB FOR bootcd)
