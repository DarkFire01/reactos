remove_definitions(-D_WIN32_WINNT=0x600 -DWINVER=0x600)
add_definitions(-D_WIN32_WINNT=0x502)

add_subdirectory(glu32)
add_subdirectory(mesa)
add_subdirectory(opengl32)
