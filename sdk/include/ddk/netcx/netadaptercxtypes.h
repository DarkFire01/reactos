/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NetAdapterCx object handles and common types
 */

#pragma once

#include <net/returncontexttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

DECLARE_HANDLE(NETADAPTER);
DECLARE_HANDLE(NETCONFIGURATION);
DECLARE_HANDLE(NETOFFLOAD);
DECLARE_HANDLE(NETPACKETQUEUE);
DECLARE_HANDLE(NETWAKESOURCE);
DECLARE_HANDLE(NETPOWEROFFLOAD);

struct _NETADAPTER_INIT;
typedef struct _NETADAPTER_INIT NETADAPTER_INIT;

typedef union _NET_EUI48_ADDRESS
{
    UINT8 Value[6];
} NET_EUI48_ADDRESS;

typedef struct _NET_DRIVER_GLOBALS
{
    ULONG Unused;
} NET_DRIVER_GLOBALS, *PNET_DRIVER_GLOBALS;

/* The class extension is reached through this table, see netfuncenum.h. */
typedef VOID (*NETFUNC)(VOID);
extern NETFUNC NetFunctions[];

typedef union _NET_IPV4_ADDRESS
{
    UINT32 Address;
    UINT8 Value[4];
} NET_IPV4_ADDRESS;

C_ASSERT(sizeof(NET_IPV4_ADDRESS) == 4);

typedef union _NET_IPV6_ADDRESS
{
    struct
    {
        UINT64 NetworkPrefix;
        UINT64 InterfaceIdentifier;
    } Unicast;
    UINT8 Value[16];
} NET_IPV6_ADDRESS;

C_ASSERT(sizeof(NET_IPV6_ADDRESS) == 16);

#ifdef __cplusplus
}
#endif
