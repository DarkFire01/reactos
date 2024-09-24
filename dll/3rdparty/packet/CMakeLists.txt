
spec2def(packet.dll packet.spec ADD_IMPORTLIB)
add_definitions(
    -DWIN32
    -D_WINDOWS
    -DHAVE_IPHELPER_API
    -DPACKET_DLL)

include_directories(
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/libs/winpcap
    ${REACTOS_SOURCE_DIR}/dll/3rdparty/packet)

list(APPEND SOURCE
    AdInfo.c
    NpfImExt.c
    Packet32.c)

add_library(packet MODULE ${SOURCE} ${CMAKE_CURRENT_BINARY_DIR}/packet.def)
set_module_type(packet win32dll)
add_importlibs(packet version iphlpapi ws2_32 msvcrt advapi32 kernel32 ntdll)
#add_pch(packet precomp.h SOURCE)
add_cd_file(TARGET packet DESTINATION reactos/system32 FOR all)
