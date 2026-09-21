/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Offsets a debugger extension needs to walk the class extension
 *
 * The block is a global so a dump reader can find it by symbol and read the
 * member offsets of this particular build instead of hardcoding them.
 */

#pragma once

#define NETADAPTERCX_TRIAGE_SIGNATURE       0x9ADCBFE0
#define NETADAPTERCX_TRIAGE_VERSION_2       2

/* Subcode of the live kernel dump taken for a platform level device reset. */
#define NETADAPTERCX_BUGCHECK_DEVICE_RESET  0x54

typedef struct DECLSPEC_ALIGN(8) _NETADAPTERCX_GLOBAL_TRIAGE_BLOCK
{
    ULONG Signature;
    USHORT Version;
    USHORT Size;
    USHORT StateMachineEngineOffset;
    USHORT NxDeviceAdapterCollectionOffset;
    USHORT NxAdapterCollectionCountOffset;
    USHORT NxAdapterLinkageOffset;
    ULONG64 ResetDiagnosticsStartAddr;
    ULONG64 ResetDiagnosticsSize;
} NETADAPTERCX_GLOBAL_TRIAGE_BLOCK;

extern NETADAPTERCX_GLOBAL_TRIAGE_BLOCK g_NetAdapterCxTriageBlock;
