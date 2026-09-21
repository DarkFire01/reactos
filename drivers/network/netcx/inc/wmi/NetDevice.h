/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     WMI event raised when a device changes power state
 *
 * The data block the class extension fills in, as described by the
 * NetAdapterMof resource it registers alongside the NDIS GUIDs.
 */

#pragma once

DEFINE_GUID(GUID_NETCX_DEVICE_POWER_STATE_CHANGE,
            0x57a60f36, 0x7b95, 0x4799, 0x9e, 0xfb, 0xd1, 0x69, 0xe6, 0xea, 0x03, 0x97);

typedef struct _NETCX_DEVICE_POWER_STATE_CHANGE
{
    DEVICE_POWER_STATE TargetState;
    POWER_ACTION PowerAction;
} NETCX_DEVICE_POWER_STATE_CHANGE, *PNETCX_DEVICE_POWER_STATE_CHANGE;
