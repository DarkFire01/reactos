add_library(qcomqmi
    codec.c
    service.c
    client.c
    transport_ipc.c)
add_dependencies(qcomqmi xdk)
