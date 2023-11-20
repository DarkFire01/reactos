
add_subdirectory(d3d8thk)
add_subdirectory(ksuser)
#add_subdirectory(ksproxy) #Not needed at the moment

if (NATIVE_REACTX)
    #add_subdirectory(bdaplgin) #disabled in trunk
    add_subdirectory(d3d8) #disabled in trunk, see wine subfolder
    add_subdirectory(d3d9) #disabled in trunk, see wine subfolder
    add_subdirectory(ddraw) #disabled in trunk, see wine subfolder
    add_subdirectory(dsound_new) #disabled in trunk
    #add_subdirectory(msdvbnp) #disabled in trunk
    #add_subdirectory(msvidctl) #disabled in trunk
endif()

if (NOT NATIVE_REACTX)
    add_subdirectory(wine)
endif()