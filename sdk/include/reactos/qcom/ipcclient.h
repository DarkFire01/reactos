/*
 * PROJECT:     ReactOS Qualcomm SoC support
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Client facing ABI of the IPC router device \Device\IPC
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/*
 * The contract a client sees on \Device\IPC. One open handle is one port.
 * Shared so the router and its kernel clients agree on it in one place.
 */

#define IPC_DEVICE_NAME             L"\\Device\\IPC"

/* A routable endpoint, node and port */
typedef struct _IPC_ADDR
{
    ULONG Node;
    ULONG Port;
} IPC_ADDR, *PIPC_ADDR;

/* Read returns the sender address then the body, write takes the destination
 * address then the body. This prefix is sizeof(IPC_ADDR). */
#define IPC_ADDR_PREFIX             (sizeof(IPC_ADDR))

/* A control event is five little endian u32 {Event, Node, Port, 0, 0} */
#define IPC_CONTROL_SIZE            20

#define IPC_EVENT_NEW_SERVER        0
#define IPC_EVENT_DATA              1
#define IPC_EVENT_REMOVE_SERVER     2
#define IPC_EVENT_REMOVE_CLIENT     3
#define IPC_EVENT_RESUME_TX         4

/* Control IOCTLs, FILE_DEVICE_NETWORK function codes 0x800.. */
#define IPC_IOCTL_GET_EVENT         CTL_CODE(FILE_DEVICE_NETWORK, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IPC_IOCTL_POLL_EVENT        CTL_CODE(FILE_DEVICE_NETWORK, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IPC_IOCTL_LOOKUP_SERVER     CTL_CODE(FILE_DEVICE_NETWORK, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IPC_IOCTL_BIND_NAME         CTL_CODE(FILE_DEVICE_NETWORK, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IPC_IOCTL_UNBIND_NAME       CTL_CODE(FILE_DEVICE_NETWORK, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IPC_IOCTL_GET_SELF          CTL_CODE(FILE_DEVICE_NETWORK, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IPC_IOCTL_SET_RX_ENABLED    CTL_CODE(FILE_DEVICE_NETWORK, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* BIND_NAME, UNBIND_NAME and LOOKUP_SERVER carry a name as two u32 */
typedef struct _IPC_NAME
{
    ULONG Service;
    ULONG Instance;
} IPC_NAME, *PIPC_NAME;

C_ASSERT(sizeof(IPC_ADDR) == 8);
C_ASSERT(sizeof(IPC_NAME) == 8);
