
# Build at the newest level so the version gated GUIDs are instantiated too
remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(-D_WIN32_WINNT=0xA00 -DNTDDI_VERSION=0x0A000008)

add_library(wdmguid wdmguid.c)
