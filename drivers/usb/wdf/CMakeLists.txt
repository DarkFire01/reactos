if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ucx01000")
    add_subdirectory(ucx01000)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/usbhub3")
    add_subdirectory(usbhub3)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/usbxhci")
    add_subdirectory(usbxhci)
endif()
