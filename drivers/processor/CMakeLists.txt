
# The vendor power drivers program MSRs and MWAIT, x86 and x64 only
if(ARCH STREQUAL "i386" OR ARCH STREQUAL "amd64")
    add_subdirectory(amdppm)
    add_subdirectory(intelppm)
    add_subdirectory(ppmlib)
endif()
add_subdirectory(processr)
