/*
 * PROJECT:     ReactOS Qualcomm audio (SDM845)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Topology miniport exposing the speaker endpoint
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"
#include "tables.hpp"

/* One speaker per system, so the control state lives with the driver */
static LONG TopoVolume = QCA_VOLUME_MAX;
static BOOLEAN TopoMuted = FALSE;

#ifdef _MSC_VER
#pragma code_seg("PAGE")
#endif

NTSTATUS
NTAPI
CreateMiniportTopologyQcom(PUNKNOWN *Unknown, REFCLSID, PUNKNOWN OuterUnknown, POOL_TYPE PoolType)
{
    PAGED_CODE();
    STD_CREATE_BODY_(CMiniportTopologyQcom, Unknown, OuterUnknown, PoolType, PMINIPORTTOPOLOGY);
}

CMiniportTopologyQcom::~CMiniportTopologyQcom()
{
    PAGED_CODE();
}

STDMETHODIMP
CMiniportTopologyQcom::NonDelegatingQueryInterface(REFIID Interface, PVOID *Object)
{
    PAGED_CODE();

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
        *Object = PVOID(PUNKNOWN(PMINIPORTTOPOLOGY(this)));
    else if (IsEqualGUIDAligned(Interface, IID_IMiniport))
        *Object = PVOID(PMINIPORT(this));
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportTopology))
        *Object = PVOID(PMINIPORTTOPOLOGY(this));
    else
        *Object = NULL;

    if (*Object)
    {
        PUNKNOWN(*Object)->AddRef();
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_PARAMETER;
}

STDMETHODIMP_(NTSTATUS)
CMiniportTopologyQcom::GetDescription(PPCFILTER_DESCRIPTOR *Description)
{
    PAGED_CODE();
    *Description = &TopoMiniportFilterDescriptor;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportTopologyQcom::DataRangeIntersection(ULONG, PKSDATARANGE, PKSDATARANGE, ULONG, PVOID, PULONG)
{
    PAGED_CODE();
    return STATUS_NOT_IMPLEMENTED;
}

STDMETHODIMP_(NTSTATUS)
CMiniportTopologyQcom::Init(PUNKNOWN, PRESOURCELIST, PPORTTOPOLOGY)
{
    PAGED_CODE();
    return STATUS_SUCCESS;
}

/* The speaker is mono, so only channel 0 or the all channels wildcard (-1) is valid */
static
BOOLEAN
TopoChannelValid(
    _In_ PPCPROPERTY_REQUEST Request)
{
    LONG Channel;

    if (Request->InstanceSize < sizeof(LONG))
        return FALSE;

    Channel = *(PLONG)Request->Instance;
    return Channel == 0 || Channel == -1;
}

/* Fills the basic support answer: access flags, then the type, then an optional range */
NTSTATUS
TopoBasicSupport(
    _In_ PPCPROPERTY_REQUEST Request,
    _In_ ULONG VariantType,
    _In_ BOOLEAN Ranged)
{
    ULONG Full = sizeof(KSPROPERTY_DESCRIPTION);
    PKSPROPERTY_DESCRIPTION Description;

    if (Ranged)
        Full += sizeof(KSPROPERTY_MEMBERSHEADER) + sizeof(KSPROPERTY_STEPPING_LONG);

    if (Request->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION))
    {
        Description = (PKSPROPERTY_DESCRIPTION)Request->Value;
        RtlZeroMemory(Description, sizeof(*Description));
        Description->AccessFlags = KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET;
        Description->DescriptionSize = Full;
        Description->PropTypeSet.Set = KSPROPTYPESETID_General;
        Description->PropTypeSet.Id = VariantType;
        Description->MembersListCount = Ranged ? 1 : 0;

        if (Ranged && Request->ValueSize >= Full)
        {
            PKSPROPERTY_MEMBERSHEADER Members = (PKSPROPERTY_MEMBERSHEADER)(Description + 1);
            PKSPROPERTY_STEPPING_LONG Range = (PKSPROPERTY_STEPPING_LONG)(Members + 1);

            Members->MembersFlags = KSPROPERTY_MEMBER_STEPPEDRANGES;
            Members->MembersSize = sizeof(*Range);
            Members->MembersCount = 1;
            Members->Flags = 0;
            Range->SteppingDelta = QCA_VOLUME_STEP;
            Range->Reserved = 0;
            Range->Bounds.SignedMinimum = QCA_VOLUME_MIN;
            Range->Bounds.SignedMaximum = QCA_VOLUME_MAX;
            Request->ValueSize = Full;
        }
        else
        {
            Request->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
        }
        return STATUS_SUCCESS;
    }

    if (Request->ValueSize >= sizeof(ULONG))
    {
        *(PULONG)Request->Value = KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET;
        Request->ValueSize = sizeof(ULONG);
        return STATUS_SUCCESS;
    }

    return STATUS_BUFFER_TOO_SMALL;
}

NTSTATUS
NTAPI
TopoPropertyVolume(PPCPROPERTY_REQUEST Request)
{
    LONG Level;

    PAGED_CODE();

    if (Request->Node != NODE_TOPO_VOLUME)
        return STATUS_INVALID_PARAMETER;

    if (Request->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
        return TopoBasicSupport(Request, VT_I4, TRUE);

    if (!TopoChannelValid(Request))
        return STATUS_INVALID_PARAMETER;
    if (Request->ValueSize < sizeof(LONG))
        return STATUS_BUFFER_TOO_SMALL;

    if (Request->Verb & KSPROPERTY_TYPE_GET)
    {
        *(PLONG)Request->Value = TopoVolume;
        Request->ValueSize = sizeof(LONG);
        return STATUS_SUCCESS;
    }

    if (Request->Verb & KSPROPERTY_TYPE_SET)
    {
        Level = *(PLONG)Request->Value;
        if (Level < QCA_VOLUME_MIN)
            Level = QCA_VOLUME_MIN;
        if (Level > QCA_VOLUME_MAX)
            Level = QCA_VOLUME_MAX;

        TopoVolume = Level;
        QcAmpSetVolume(Level);
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}

NTSTATUS
NTAPI
TopoPropertyMute(PPCPROPERTY_REQUEST Request)
{
    PAGED_CODE();

    if (Request->Node != NODE_TOPO_MUTE)
        return STATUS_INVALID_PARAMETER;

    if (Request->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
        return TopoBasicSupport(Request, VT_BOOL, FALSE);

    if (!TopoChannelValid(Request))
        return STATUS_INVALID_PARAMETER;
    if (Request->ValueSize < sizeof(BOOL))
        return STATUS_BUFFER_TOO_SMALL;

    if (Request->Verb & KSPROPERTY_TYPE_GET)
    {
        *(PBOOL)Request->Value = TopoMuted;
        Request->ValueSize = sizeof(BOOL);
        return STATUS_SUCCESS;
    }

    if (Request->Verb & KSPROPERTY_TYPE_SET)
    {
        TopoMuted = *(PBOOL)Request->Value ? TRUE : FALSE;
        QcAmpSetMute(TopoMuted);
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}

#ifdef _MSC_VER
#pragma code_seg()
#endif
