/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Private view of the network adapter object
 */

#pragma once

#include <netcx/netadapter.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
NDIS_HANDLE
(NTAPI *PFN_NETADAPTERWDMGETNDISHANDLE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter);

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
NDIS_HANDLE
NTAPI
NetAdapterWdmGetNdisHandle(
    _In_ NETADAPTER Adapter)
{
    return ((PFN_NETADAPTERWDMGETNDISHANDLE)NetFunctions[NetAdapterWdmGetNdisHandleTableIndex])(
        NetDriverGlobals, Adapter);
}

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
NDIS_HANDLE
(NTAPI *PFN_NETADAPTERDRIVERWDMGETHANDLE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDRIVER Driver);

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
NDIS_HANDLE
NTAPI
NetAdapterDriverWdmGetHandle(
    _In_ WDFDRIVER Driver)
{
    return ((PFN_NETADAPTERDRIVERWDMGETHANDLE)NetFunctions[NetAdapterDriverWdmGetHandleTableIndex])(
        NetDriverGlobals, Driver);
}

#ifdef __cplusplus
}
#endif
