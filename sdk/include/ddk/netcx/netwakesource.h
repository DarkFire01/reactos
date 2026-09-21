/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Patterns that wake the device from a low power state
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _NET_WAKE_SOURCE_BITMAP_PARAMETERS
{
    ULONG Size;
    ULONG Id;
    const UCHAR *Pattern;
    SIZE_T PatternSize;
    const UCHAR *Mask;
    SIZE_T MaskSize;
} NET_WAKE_SOURCE_BITMAP_PARAMETERS;

typedef struct _NET_WAKE_SOURCE_MEDIA_CHANGE_PARAMETERS
{
    ULONG Size;
    BOOLEAN MediaConnect;
    BOOLEAN MediaDisconnect;
} NET_WAKE_SOURCE_MEDIA_CHANGE_PARAMETERS;

typedef enum _NET_WAKE_SOURCE_TYPE
{
    NetWakeSourceTypeBitmapPattern = 1,
    NetWakeSourceTypeMagicPacket,
    NetWakeSourceTypeMediaChange,
    NetWakeSourceTypePacketFilterMatch
} NET_WAKE_SOURCE_TYPE;

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
NET_WAKE_SOURCE_TYPE
(NTAPI *PFN_NETWAKESOURCEGETTYPE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETWAKESOURCE WakeSource);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NET_WAKE_SOURCE_TYPE
NTAPI
NetWakeSourceGetType(
    _In_ NETWAKESOURCE WakeSource)
{
    return ((PFN_NETWAKESOURCEGETTYPE)NetFunctions[NetWakeSourceGetTypeTableIndex])(
        NetDriverGlobals, WakeSource);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
NETADAPTER
(NTAPI *PFN_NETWAKESOURCEGETADAPTER)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETWAKESOURCE WakeSource);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NETADAPTER
NTAPI
NetWakeSourceGetAdapter(
    _In_ NETWAKESOURCE WakeSource)
{
    return ((PFN_NETWAKESOURCEGETADAPTER)NetFunctions[NetWakeSourceGetAdapterTableIndex])(
        NetDriverGlobals, WakeSource);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
(NTAPI *PFN_NETWAKESOURCEGETBITMAPPARAMETERS)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETWAKESOURCE WakeSource,
    _Inout_ NET_WAKE_SOURCE_BITMAP_PARAMETERS *Parameters);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetWakeSourceGetBitmapParameters(
    _In_ NETWAKESOURCE WakeSource,
    _Inout_ NET_WAKE_SOURCE_BITMAP_PARAMETERS *Parameters)
{
    ((PFN_NETWAKESOURCEGETBITMAPPARAMETERS)
        NetFunctions[NetWakeSourceGetBitmapParametersTableIndex])(
            NetDriverGlobals, WakeSource, Parameters);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
(NTAPI *PFN_NETWAKESOURCEGETMEDIACHANGEPARAMETERS)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETWAKESOURCE WakeSource,
    _Inout_ NET_WAKE_SOURCE_MEDIA_CHANGE_PARAMETERS *Parameters);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetWakeSourceGetMediaChangeParameters(
    _In_ NETWAKESOURCE WakeSource,
    _Inout_ NET_WAKE_SOURCE_MEDIA_CHANGE_PARAMETERS *Parameters)
{
    ((PFN_NETWAKESOURCEGETMEDIACHANGEPARAMETERS)
        NetFunctions[NetWakeSourceGetMediaChangeParametersTableIndex])(
            NetDriverGlobals, WakeSource, Parameters);
}

#ifdef __cplusplus
}
#endif
