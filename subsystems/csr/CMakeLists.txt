
add_subdirectory(csrlib)
if(NOT SYSWOW64_BUILD)
    add_subdirectory(csrsrv)
    add_subdirectory(csrss)
endif()
