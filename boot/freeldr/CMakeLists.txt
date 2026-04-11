
add_subdirectory(fdebug)
add_subdirectory(freeldr)

if(ARCH STREQUAL "i386" OR ARCH STREQUAL "amd64")
add_subdirectory(install)
add_subdirectory(bootsect)
elseif(ARCH STREQUAL "arm" OR ARCH STREQUAL "arm64")
# FAT VBR for efisys (El Torito EFI floppy); x86 16-bit sector, built with host ml.exe
add_subdirectory(bootsect)
endif()
