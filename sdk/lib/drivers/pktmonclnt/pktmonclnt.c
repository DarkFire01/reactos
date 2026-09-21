/*
 * PROJECT:     ReactOS
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Packet monitor client library, with no monitor to talk to
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * The real library attaches to the packet monitor provider over NMR. There is
 * no provider here, so every call behaves the way the real one does while the
 * provider is absent: initialization succeeds, anything that needs the
 * provider reports it is not ready, and nothing is ever logged. Callers
 * already treat a failed registration as "monitoring unavailable".
 */

#include <ntddk.h>
#include <pktmonclnt.h>

#define NDEBUG
#include <debug.h>

_Use_decl_annotations_
NTSTATUS
PktMonClientInitializeEx(
    const NPI_MODULEID *ModuleId,
    PKTMON_CLIENT_ENUMERATE_CALLBACK EnumerateAndRegister,
    PKTMON_CLIENT_ENUMERATE_CALLBACK EnumerateAndUnregister)
{
    UNREFERENCED_PARAMETER(ModuleId);
    UNREFERENCED_PARAMETER(EnumerateAndRegister);
    UNREFERENCED_PARAMETER(EnumerateAndUnregister);

    /* The provider never attaches, so neither callback ever runs. */
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
PktMonClientUninitialize(
    VOID)
{
}

_Use_decl_annotations_
NTSTATUS
PktMonClientComponentRegister(
    PKTMON_COMPONENT_CONTEXT *CompContext,
    PUNICODE_STRING Name,
    PUNICODE_STRING Description,
    PKTMON_COMPONENT_TYPE Type,
    NDIS_MEDIUM MediaType,
    PKTMON_DIRECTION_TAG DirTagIn,
    PKTMON_DIRECTION_TAG DirTagOut)
{
    UNREFERENCED_PARAMETER(CompContext);
    UNREFERENCED_PARAMETER(Name);
    UNREFERENCED_PARAMETER(Description);
    UNREFERENCED_PARAMETER(Type);
    UNREFERENCED_PARAMETER(MediaType);
    UNREFERENCED_PARAMETER(DirTagIn);
    UNREFERENCED_PARAMETER(DirTagOut);

    return STATUS_DEVICE_NOT_READY;
}

/* Never registered, so there is no handle and nothing to take down. */
_Use_decl_annotations_
VOID
PktMonClientComponentUnregister(
    PKTMON_COMPONENT_CONTEXT *CompContext)
{
    UNREFERENCED_PARAMETER(CompContext);
}

_Use_decl_annotations_
NTSTATUS
PktMonClientAddEdge(
    PKTMON_COMPONENT_CONTEXT *CompContext,
    PUNICODE_STRING Name,
    PKTMON_DIRECTION_TAG DirTagIn,
    PKTMON_DIRECTION_TAG DirTagOut,
    NDIS_MEDIUM MediaType,
    PKTMON_EDGE_CONTEXT *EdgeContext)
{
    UNREFERENCED_PARAMETER(CompContext);
    UNREFERENCED_PARAMETER(Name);
    UNREFERENCED_PARAMETER(DirTagIn);
    UNREFERENCED_PARAMETER(DirTagOut);
    UNREFERENCED_PARAMETER(MediaType);
    UNREFERENCED_PARAMETER(EdgeContext);

    return STATUS_DEVICE_NOT_READY;
}

_Use_decl_annotations_
NTSTATUS
PktMonClientSetCompProperty(
    PKTMON_COMPONENT_CONTEXT *CompContext,
    PKTMON_COMPONENT_PROPERTY_ID Id,
    PVOID Value,
    USHORT Size)
{
    UNREFERENCED_PARAMETER(CompContext);
    UNREFERENCED_PARAMETER(Id);
    UNREFERENCED_PARAMETER(Value);
    UNREFERENCED_PARAMETER(Size);

    return STATUS_DEVICE_NOT_READY;
}

_Use_decl_annotations_
VOID
PktMonClientNblLog(
    PKTMON_EDGE_CONTEXT *EdgeContext,
    NET_BUFFER_LIST *NetBufferList,
    PKTMON_PACKET_TYPE PacketType,
    struct _PKTMON_PACKET_HEADER_INFO *PacketHeaderInfo,
    BOOLEAN UseOnlyFirstNbl,
    PKTMON_DIRECTION Direction)
{
    UNREFERENCED_PARAMETER(EdgeContext);
    UNREFERENCED_PARAMETER(NetBufferList);
    UNREFERENCED_PARAMETER(PacketType);
    UNREFERENCED_PARAMETER(PacketHeaderInfo);
    UNREFERENCED_PARAMETER(UseOnlyFirstNbl);
    UNREFERENCED_PARAMETER(Direction);
}

_Use_decl_annotations_
VOID
PktMonClientNblDrop(
    PKTMON_COMPONENT_CONTEXT *CompContext,
    NET_BUFFER_LIST *NetBufferList,
    PKTMON_PACKET_TYPE PacketType,
    struct _PKTMON_PACKET_HEADER_INFO *PacketHeaderInfo,
    BOOLEAN UseOnlyFirstNbl,
    PKTMON_DIRECTION Direction,
    PKTMON_DROP_REASON DropReason,
    ULONG LocationCode)
{
    UNREFERENCED_PARAMETER(CompContext);
    UNREFERENCED_PARAMETER(NetBufferList);
    UNREFERENCED_PARAMETER(PacketType);
    UNREFERENCED_PARAMETER(PacketHeaderInfo);
    UNREFERENCED_PARAMETER(UseOnlyFirstNbl);
    UNREFERENCED_PARAMETER(Direction);
    UNREFERENCED_PARAMETER(DropReason);
    UNREFERENCED_PARAMETER(LocationCode);
}
