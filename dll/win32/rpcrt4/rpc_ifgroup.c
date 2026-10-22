/*
 * PROJECT:     ReactOS RPC Runtime
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Server interface groups
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * Groups are built on the regular registration, protseq and endpoint mapper calls.
 * Endpoints stay open after deactivation, so calls into an inactive group get an unknown interface fault.
 */

#include "precomp.h"

WINE_DEFAULT_DEBUG_CHANNEL(rpc);

NTSYSAPI
ULONG
NTAPI
RtlLengthSecurityDescriptor(
    _In_ PSECURITY_DESCRIPTOR SecurityDescriptor);

#define IFGROUP_SIGNATURE 0x70476649

/* Endpoint template flag: the endpoint is never published in the endpoint mapper */
#define IFGROUP_ENDPOINT_NO_EPM 0x8000

typedef enum _IFGROUP_STATE
{
    IfGroupInactive,
    IfGroupActive,
    IfGroupClosed
} IFGROUP_STATE;

typedef struct _IFGROUP_INTERFACE
{
    RPC_INTERFACE_TEMPLATEW Template;
    UUID MgrTypeUuid;
} IFGROUP_INTERFACE, *PIFGROUP_INTERFACE;

typedef struct _IFGROUP_ENDPOINT
{
    RPC_ENDPOINT_TEMPLATEW Template;
    LPSTR ProtSeqA;
    LPSTR EndpointA;
} IFGROUP_ENDPOINT, *PIFGROUP_ENDPOINT;

typedef struct _IFGROUP
{
    ULONG Signature;
    IFGROUP_STATE State;
    CRITICAL_SECTION Lock;

    ULONG InterfaceCount;
    PIFGROUP_INTERFACE Interfaces;
    ULONG EndpointCount;
    PIFGROUP_ENDPOINT Endpoints;

    /* Progress of the current activation, undone in reverse */
    ULONG RegisteredCount;
    ULONG PublishedCount;
    RPC_BINDING_VECTOR *Bindings;

    ULONG IdlePeriod;
    RPC_INTERFACE_GROUP_IDLE_CALLBACK_FN *IdleCallback;
    PVOID IdleContext;
    HANDLE IdleTimer;
    BOOL ReportedIdle;
    LONG LastTotalCalls;
    DWORD ActiveTick;
} IFGROUP, *PIFGROUP;

static
RPC_STATUS
IfGroupCopySecurityDescriptor(
    _In_opt_ PSECURITY_DESCRIPTOR Source,
    _Outptr_result_maybenull_ PSECURITY_DESCRIPTOR *Copy)
{
    SECURITY_DESCRIPTOR_CONTROL Control;
    DWORD Revision;
    DWORD Length = 0;
    PSECURITY_DESCRIPTOR Buffer;

    *Copy = NULL;
    if (!Source)
        return RPC_S_OK;

    if (!GetSecurityDescriptorControl(Source, &Control, &Revision))
        return GetLastError();

    if (Control & SE_SELF_RELATIVE)
    {
        Length = RtlLengthSecurityDescriptor(Source);
        Buffer = malloc(Length);
        if (!Buffer)
            return RPC_S_OUT_OF_MEMORY;

        memcpy(Buffer, Source, Length);
        *Copy = Buffer;
        return RPC_S_OK;
    }

    /* Absolute form: the first call only reports the size */
    MakeSelfRelativeSD(Source, NULL, &Length);
    Buffer = malloc(Length);
    if (!Buffer)
        return RPC_S_OUT_OF_MEMORY;

    if (!MakeSelfRelativeSD(Source, Buffer, &Length))
    {
        free(Buffer);
        return GetLastError();
    }

    *Copy = Buffer;
    return RPC_S_OK;
}

static
RPC_STATUS
IfGroupCopyUuidVector(
    _In_opt_ UUID_VECTOR *Source,
    _Outptr_result_maybenull_ UUID_VECTOR **Copy)
{
    UUID_VECTOR *Vector;
    UUID *Storage;
    ULONG Index;

    *Copy = NULL;
    if (!Source)
        return RPC_S_OK;

    Vector = malloc(sizeof(*Vector) + Source->Count * (sizeof(UUID *) + sizeof(UUID)));
    if (!Vector)
        return RPC_S_OUT_OF_MEMORY;

    /* The UUIDs live right after the pointer array */
    Storage = (UUID *)&Vector->Uuid[Source->Count];
    Vector->Count = Source->Count;
    for (Index = 0; Index < Source->Count; Index++)
    {
        if (Source->Uuid[Index])
        {
            Storage[Index] = *Source->Uuid[Index];
            Vector->Uuid[Index] = &Storage[Index];
        }
        else
        {
            Vector->Uuid[Index] = NULL;
        }
    }

    *Copy = Vector;
    return RPC_S_OK;
}

static
RPC_WSTR
IfGroupDuplicateString(
    _In_ RPC_WSTR Source)
{
    SIZE_T Size = (lstrlenW(Source) + 1) * sizeof(WCHAR);
    RPC_WSTR Copy;

    Copy = malloc(Size);
    if (Copy)
        memcpy(Copy, Source, Size);

    return Copy;
}

static
VOID
IfGroupFree(
    _In_ PIFGROUP Group)
{
    ULONG Index;

    if (Group->Interfaces)
    {
        for (Index = 0; Index < Group->InterfaceCount; Index++)
        {
            free(Group->Interfaces[Index].Template.UuidVector);
            free(Group->Interfaces[Index].Template.Annotation);
            free(Group->Interfaces[Index].Template.SecurityDescriptor);
        }
        free(Group->Interfaces);
    }

    if (Group->Endpoints)
    {
        for (Index = 0; Index < Group->EndpointCount; Index++)
        {
            free(Group->Endpoints[Index].Template.ProtSeq);
            free(Group->Endpoints[Index].Template.Endpoint);
            free(Group->Endpoints[Index].Template.SecurityDescriptor);
            free(Group->Endpoints[Index].ProtSeqA);
            free(Group->Endpoints[Index].EndpointA);
        }
        free(Group->Endpoints);
    }

    DeleteCriticalSection(&Group->Lock);
    free(Group);
}

static
RPC_STATUS
IfGroupCopyInterface(
    _Out_ PIFGROUP_INTERFACE Entry,
    _In_ RPC_INTERFACE_TEMPLATEW *Source)
{
    RPC_STATUS Status;

    Entry->Template = *Source;
    Entry->Template.UuidVector = NULL;
    Entry->Template.Annotation = NULL;
    Entry->Template.SecurityDescriptor = NULL;

    if (Source->MgrTypeUuid)
    {
        Entry->MgrTypeUuid = *Source->MgrTypeUuid;
        Entry->Template.MgrTypeUuid = &Entry->MgrTypeUuid;
    }

    if (Source->Annotation)
    {
        Entry->Template.Annotation = IfGroupDuplicateString(Source->Annotation);
        if (!Entry->Template.Annotation)
            return RPC_S_OUT_OF_MEMORY;
    }

    Status = IfGroupCopyUuidVector(Source->UuidVector, &Entry->Template.UuidVector);
    if (Status != RPC_S_OK)
        return Status;

    return IfGroupCopySecurityDescriptor(Source->SecurityDescriptor,
                                         &Entry->Template.SecurityDescriptor);
}

static
RPC_STATUS
IfGroupCopyEndpoint(
    _Out_ PIFGROUP_ENDPOINT Entry,
    _In_ RPC_ENDPOINT_TEMPLATEW *Source)
{
    Entry->Template = *Source;
    Entry->Template.ProtSeq = IfGroupDuplicateString(Source->ProtSeq);
    Entry->Template.Endpoint = NULL;
    Entry->Template.SecurityDescriptor = NULL;
    Entry->ProtSeqA = RPCRT4_strdupWtoA(Source->ProtSeq);
    Entry->EndpointA = NULL;
    if (!Entry->Template.ProtSeq || !Entry->ProtSeqA)
        return RPC_S_OUT_OF_MEMORY;

    if (Source->Endpoint)
    {
        Entry->Template.Endpoint = IfGroupDuplicateString(Source->Endpoint);
        Entry->EndpointA = RPCRT4_strdupWtoA(Source->Endpoint);
        if (!Entry->Template.Endpoint || !Entry->EndpointA)
            return RPC_S_OUT_OF_MEMORY;
    }

    return IfGroupCopySecurityDescriptor(Source->SecurityDescriptor,
                                         &Entry->Template.SecurityDescriptor);
}

static
PIFGROUP
IfGroupFromHandle(
    _In_opt_ RPC_INTERFACE_GROUP IfGroup)
{
    PIFGROUP Group = IfGroup;

    if (!Group || Group->Signature != IFGROUP_SIGNATURE)
        return NULL;

    return Group;
}

/**
 * @brief Finds the group endpoint a server binding was created for.
 */
static
PIFGROUP_ENDPOINT
IfGroupMatchBinding(
    _In_ PIFGROUP Group,
    _In_ RPC_BINDING_HANDLE Binding)
{
    RpcBinding *Bind = Binding;
    PIFGROUP_ENDPOINT Entry;
    ULONG Index;

    for (Index = 0; Index < Group->EndpointCount; Index++)
    {
        Entry = &Group->Endpoints[Index];
        if (!Bind->Protseq || _stricmp(Bind->Protseq, Entry->ProtSeqA))
            continue;

        /* A dynamic endpoint claims every binding of its protocol sequence */
        if (!Entry->EndpointA)
            return Entry;

        if (Bind->Endpoint && !_stricmp(Bind->Endpoint, Entry->EndpointA))
            return Entry;
    }

    return NULL;
}

/**
 * @brief Collects the server bindings that belong to the group's endpoints.
 *
 * @param ForEndpointMapper
 * Leave out endpoints that must not be published in the endpoint mapper.
 */
static
RPC_STATUS
IfGroupQueryBindings(
    _In_ PIFGROUP Group,
    _In_ BOOL ForEndpointMapper,
    _Out_ RPC_BINDING_VECTOR **BindingVector)
{
    RPC_BINDING_VECTOR *Vector;
    PIFGROUP_ENDPOINT Entry;
    RPC_STATUS Status;
    ULONG Index;
    ULONG Kept = 0;

    *BindingVector = NULL;

    Status = RpcServerInqBindings(&Vector);
    if (Status != RPC_S_OK)
        return Status;

    /* Compact the bindings we keep to the front of the vector */
    for (Index = 0; Index < Vector->Count; Index++)
    {
        Entry = IfGroupMatchBinding(Group, Vector->BindingH[Index]);
        if (Entry && !(ForEndpointMapper && Entry->Template.Version == IFGROUP_ENDPOINT_NO_EPM))
            Vector->BindingH[Kept++] = Vector->BindingH[Index];
        else
            RpcBindingFree(&Vector->BindingH[Index]);
    }

    Vector->Count = Kept;
    if (!Kept)
    {
        free(Vector);
        return RPC_S_NO_BINDINGS;
    }

    *BindingVector = Vector;
    return RPC_S_OK;
}

/**
 * @brief Undoes what an activation set up, newest first.
 *
 * @return The status of unregistering the first interface of the group.
 */
static
RPC_STATUS
IfGroupTeardown(
    _In_ PIFGROUP Group)
{
    RPC_INTERFACE_TEMPLATEW *Template;
    RPC_STATUS Status = RPC_S_OK;

    /* Failing to withdraw a mapping does not stop the teardown */
    while (Group->PublishedCount)
    {
        Template = &Group->Interfaces[--Group->PublishedCount].Template;
        RpcEpUnregister(Template->IfSpec, Group->Bindings, Template->UuidVector);
    }

    while (Group->RegisteredCount)
    {
        Template = &Group->Interfaces[--Group->RegisteredCount].Template;
        Status = RpcServerUnregisterIf(Template->IfSpec, Template->MgrTypeUuid, TRUE);
    }

    if (Group->Bindings)
        RpcBindingVectorFree(&Group->Bindings);

    return Status;
}

/**
 * @brief Reports whether any call is running or has run since the last check.
 */
static
BOOL
IfGroupSampleActivity(
    _In_ PIFGROUP Group)
{
    RPC_INTERFACE_TEMPLATEW *Template;
    LONG Current = 0;
    LONG Total = 0;
    BOOL Active;
    ULONG Index;

    for (Index = 0; Index < Group->RegisteredCount; Index++)
    {
        Template = &Group->Interfaces[Index].Template;
        RPCRT4_query_interface_calls(Template->IfSpec, Template->MgrTypeUuid, &Current, &Total);
    }

    Active = Current || Total != Group->LastTotalCalls;
    Group->LastTotalCalls = Total;
    return Active;
}

static
VOID
CALLBACK
IfGroupIdleTimer(
    _In_ PVOID Context,
    _In_ BOOLEAN TimerFired)
{
    PIFGROUP Group = Context;
    BOOL Notify = FALSE;
    BOOL Idle = FALSE;
    DWORD Now;

    UNREFERENCED_PARAMETER(TimerFired);

    EnterCriticalSection(&Group->Lock);
    if (Group->State == IfGroupActive)
    {
        Now = GetTickCount();
        if (IfGroupSampleActivity(Group))
        {
            Group->ActiveTick = Now;
            if (Group->ReportedIdle)
            {
                Group->ReportedIdle = FALSE;
                Notify = TRUE;
            }
        }
        else if (!Group->ReportedIdle &&
                 (ULONGLONG)(Now - Group->ActiveTick) >= (ULONGLONG)Group->IdlePeriod * 1000)
        {
            Group->ReportedIdle = TRUE;
            Notify = TRUE;
            Idle = TRUE;
        }
    }
    LeaveCriticalSection(&Group->Lock);

    /* Called unlocked so the callback may deactivate the group */
    if (Notify)
        Group->IdleCallback(Group, Group->IdleContext, Idle);
}

/***********************************************************************
 *             RpcServerInterfaceGroupCreateW (RPCRT4.@)
 */
RPC_STATUS
RPC_ENTRY
RpcServerInterfaceGroupCreateW(
    _In_reads_(NumIfs) RPC_INTERFACE_TEMPLATEW *Interfaces,
    _In_ ULONG NumIfs,
    _In_reads_(NumEndpoints) RPC_ENDPOINT_TEMPLATEW *Endpoints,
    _In_ ULONG NumEndpoints,
    _In_ ULONG IdlePeriod,
    _In_opt_ RPC_INTERFACE_GROUP_IDLE_CALLBACK_FN IdleCallbackFn,
    _In_opt_ PVOID IdleCallbackContext,
    _Out_ PRPC_INTERFACE_GROUP IfGroup)
{
    PIFGROUP Group;
    RPC_STATUS Status = RPC_S_OK;
    ULONG Index;

    TRACE("(%p %lu %p %lu %lu %p %p %p)\n", Interfaces, NumIfs, Endpoints, NumEndpoints,
          IdlePeriod, IdleCallbackFn, IdleCallbackContext, IfGroup);

    if (!NumEndpoints || !NumIfs || !IfGroup || !Endpoints || !Interfaces)
        return RPC_S_INVALID_ARG;

    /* Without a callback there is nobody to report idleness to */
    if (!IdleCallbackFn && IdlePeriod != INFINITE)
        return RPC_S_INVALID_ARG;

    /* Only the first templates have their versions checked */
    if ((Endpoints[0].Version & ~IFGROUP_ENDPOINT_NO_EPM) || Interfaces[0].Version)
        return RPC_S_INVALID_ARG;

    for (Index = 0; Index < NumEndpoints; Index++)
    {
        if (!Endpoints[Index].ProtSeq ||
            !wcscmp(Endpoints[Index].ProtSeq, L"ncadg_ip_udp") ||
            !wcscmp(Endpoints[Index].ProtSeq, L"ncacn_http"))
        {
            return RPC_S_INVALID_ARG;
        }

        /* An unpublished endpoint must be well known to be reachable */
        if (Endpoints[Index].Version == IFGROUP_ENDPOINT_NO_EPM &&
            (!Endpoints[Index].Endpoint || !*Endpoints[Index].Endpoint))
        {
            return RPC_S_INVALID_ARG;
        }
    }

    Group = calloc(1, sizeof(*Group));
    if (!Group)
        return RPC_S_OUT_OF_MEMORY;

    InitializeCriticalSection(&Group->Lock);
    Group->State = IfGroupInactive;
    Group->IdlePeriod = IdlePeriod;
    Group->IdleCallback = IdleCallbackFn;
    Group->IdleContext = IdleCallbackContext;

    Group->Interfaces = calloc(NumIfs, sizeof(*Group->Interfaces));
    Group->Endpoints = calloc(NumEndpoints, sizeof(*Group->Endpoints));
    if (!Group->Interfaces || !Group->Endpoints)
    {
        IfGroupFree(Group);
        return RPC_S_OUT_OF_MEMORY;
    }

    /* The counts track how many entries hold copies, for cleanup */
    while (Group->InterfaceCount < NumIfs && Status == RPC_S_OK)
    {
        Status = IfGroupCopyInterface(&Group->Interfaces[Group->InterfaceCount],
                                      &Interfaces[Group->InterfaceCount]);
        Group->InterfaceCount++;
    }

    while (Group->EndpointCount < NumEndpoints && Status == RPC_S_OK)
    {
        Status = IfGroupCopyEndpoint(&Group->Endpoints[Group->EndpointCount],
                                     &Endpoints[Group->EndpointCount]);
        Group->EndpointCount++;
    }

    if (Status == RPC_S_OK && IdleCallbackFn)
    {
        if (!CreateTimerQueueTimer(&Group->IdleTimer,
                                   NULL,
                                   IfGroupIdleTimer,
                                   Group,
                                   1000,
                                   1000,
                                   WT_EXECUTEDEFAULT))
        {
            Group->IdleTimer = NULL;
            Status = RPC_S_OUT_OF_RESOURCES;
        }
    }

    if (Status != RPC_S_OK)
    {
        IfGroupFree(Group);
        return Status;
    }

    Group->Signature = IFGROUP_SIGNATURE;
    *IfGroup = Group;
    return RPC_S_OK;
}

/***********************************************************************
 *             RpcServerInterfaceGroupCreateA (RPCRT4.@)
 */
RPC_STATUS
RPC_ENTRY
RpcServerInterfaceGroupCreateA(
    _In_reads_(NumIfs) RPC_INTERFACE_TEMPLATEA *Interfaces,
    _In_ ULONG NumIfs,
    _In_reads_(NumEndpoints) RPC_ENDPOINT_TEMPLATEA *Endpoints,
    _In_ ULONG NumEndpoints,
    _In_ ULONG IdlePeriod,
    _In_opt_ RPC_INTERFACE_GROUP_IDLE_CALLBACK_FN IdleCallbackFn,
    _In_opt_ PVOID IdleCallbackContext,
    _Out_ PRPC_INTERFACE_GROUP IfGroup)
{
    RPC_INTERFACE_TEMPLATEW *InterfacesW = NULL;
    RPC_ENDPOINT_TEMPLATEW *EndpointsW = NULL;
    RPC_STATUS Status = RPC_S_OUT_OF_MEMORY;
    ULONG Index;

    if (!Interfaces || !Endpoints)
        return RPC_S_INVALID_ARG;

    InterfacesW = calloc(NumIfs, sizeof(*InterfacesW));
    EndpointsW = calloc(NumEndpoints, sizeof(*EndpointsW));
    if ((NumIfs && !InterfacesW) || (NumEndpoints && !EndpointsW))
        goto Cleanup;

    /* The template layouts only differ in their string types */
    for (Index = 0; Index < NumIfs; Index++)
    {
        memcpy(&InterfacesW[Index], &Interfaces[Index], sizeof(InterfacesW[Index]));
        InterfacesW[Index].Annotation = RPCRT4_strdupAtoW((LPCSTR)Interfaces[Index].Annotation);
        if (Interfaces[Index].Annotation && !InterfacesW[Index].Annotation)
            goto Cleanup;
    }

    for (Index = 0; Index < NumEndpoints; Index++)
    {
        memcpy(&EndpointsW[Index], &Endpoints[Index], sizeof(EndpointsW[Index]));
        EndpointsW[Index].ProtSeq = RPCRT4_strdupAtoW((LPCSTR)Endpoints[Index].ProtSeq);
        EndpointsW[Index].Endpoint = RPCRT4_strdupAtoW((LPCSTR)Endpoints[Index].Endpoint);
        if ((Endpoints[Index].ProtSeq && !EndpointsW[Index].ProtSeq) ||
            (Endpoints[Index].Endpoint && !EndpointsW[Index].Endpoint))
        {
            goto Cleanup;
        }
    }

    Status = RpcServerInterfaceGroupCreateW(InterfacesW,
                                            NumIfs,
                                            EndpointsW,
                                            NumEndpoints,
                                            IdlePeriod,
                                            IdleCallbackFn,
                                            IdleCallbackContext,
                                            IfGroup);

Cleanup:
    for (Index = 0; InterfacesW && Index < NumIfs; Index++)
        free(InterfacesW[Index].Annotation);

    for (Index = 0; EndpointsW && Index < NumEndpoints; Index++)
    {
        free(EndpointsW[Index].ProtSeq);
        free(EndpointsW[Index].Endpoint);
    }

    free(InterfacesW);
    free(EndpointsW);
    return Status;
}

/***********************************************************************
 *             RpcServerInterfaceGroupActivate (RPCRT4.@)
 */
RPC_STATUS
RPC_ENTRY
RpcServerInterfaceGroupActivate(
    _In_ RPC_INTERFACE_GROUP IfGroup)
{
    PIFGROUP Group = IfGroupFromHandle(IfGroup);
    RPC_INTERFACE_TEMPLATEW *Template;
    RPC_ENDPOINT_TEMPLATEW *Endpoint;
    RPC_STATUS Status = RPC_S_OK;
    ULONG Index;

    TRACE("(%p)\n", IfGroup);

    if (!Group)
        return RPC_S_INVALID_ARG;

    EnterCriticalSection(&Group->Lock);

    if (Group->State == IfGroupActive)
    {
        Status = RPC_S_ALREADY_LISTENING;
        goto Exit;
    }

    if (Group->State == IfGroupClosed)
    {
        Status = RPC_S_INVALID_ARG;
        goto Exit;
    }

    /* Group interfaces listen on their own, without RpcServerListen */
    while (Group->RegisteredCount < Group->InterfaceCount)
    {
        Template = &Group->Interfaces[Group->RegisteredCount].Template;
        Status = RpcServerRegisterIf3(Template->IfSpec,
                                      Template->MgrTypeUuid,
                                      Template->MgrEpv,
                                      Template->Flags | RPC_IF_AUTOLISTEN,
                                      Template->MaxCalls,
                                      Template->MaxRpcSize,
                                      Template->IfCallback,
                                      Template->SecurityDescriptor);
        if (Status != RPC_S_OK)
            goto Rollback;

        Group->RegisteredCount++;
    }

    for (Index = 0; Index < Group->EndpointCount; Index++)
    {
        Endpoint = &Group->Endpoints[Index].Template;
        if (Endpoint->Endpoint)
        {
            Status = RpcServerUseProtseqEpW(Endpoint->ProtSeq,
                                            Endpoint->Backlog,
                                            Endpoint->Endpoint,
                                            Endpoint->SecurityDescriptor);
        }
        else
        {
            Status = RpcServerUseProtseqW(Endpoint->ProtSeq,
                                          Endpoint->Backlog,
                                          Endpoint->SecurityDescriptor);
        }

        if (Status != RPC_S_OK)
            goto Rollback;
    }

    /* Publish every endpoint the group may advertise */
    Status = IfGroupQueryBindings(Group, TRUE, &Group->Bindings);
    if (Status == RPC_S_OK)
    {
        while (Group->PublishedCount < Group->InterfaceCount)
        {
            Template = &Group->Interfaces[Group->PublishedCount].Template;
            Status = RpcEpRegisterW(Template->IfSpec,
                                    Group->Bindings,
                                    Template->UuidVector,
                                    Template->Annotation);
            if (Status != RPC_S_OK)
                goto Rollback;

            Group->PublishedCount++;
        }
    }
    else if (Status != RPC_S_NO_BINDINGS)
    {
        goto Rollback;
    }

    Group->State = IfGroupActive;
    Group->ReportedIdle = FALSE;
    Group->ActiveTick = GetTickCount();
    IfGroupSampleActivity(Group);
    Status = RPC_S_OK;
    goto Exit;

Rollback:
    IfGroupTeardown(Group);

Exit:
    LeaveCriticalSection(&Group->Lock);
    return Status;
}

/***********************************************************************
 *             RpcServerInterfaceGroupDeactivate (RPCRT4.@)
 */
RPC_STATUS
RPC_ENTRY
RpcServerInterfaceGroupDeactivate(
    _In_ RPC_INTERFACE_GROUP IfGroup,
    _In_ ULONG ForceDeactivation)
{
    PIFGROUP Group = IfGroupFromHandle(IfGroup);
    RPC_STATUS Status;

    TRACE("(%p %lu)\n", IfGroup, ForceDeactivation);

    if (!Group)
        return RPC_S_INVALID_ARG;

    EnterCriticalSection(&Group->Lock);

    if (Group->State != IfGroupActive)
    {
        Status = RPC_S_NOT_LISTENING;
    }
    else if (!ForceDeactivation && IfGroupSampleActivity(Group))
    {
        Status = RPC_S_SERVER_TOO_BUSY;
    }
    else
    {
        Status = IfGroupTeardown(Group);
        if (Status == RPC_S_OK)
            Group->State = IfGroupInactive;
    }

    LeaveCriticalSection(&Group->Lock);
    return Status;
}

/***********************************************************************
 *             RpcServerInterfaceGroupClose (RPCRT4.@)
 */
RPC_STATUS
RPC_ENTRY
RpcServerInterfaceGroupClose(
    _In_ RPC_INTERFACE_GROUP IfGroup)
{
    PIFGROUP Group = IfGroupFromHandle(IfGroup);
    RPC_STATUS Status = RPC_S_OK;

    TRACE("(%p)\n", IfGroup);

    if (!Group)
        return RPC_S_INVALID_ARG;

    EnterCriticalSection(&Group->Lock);

    if (Group->State == IfGroupActive)
    {
        Status = IfGroupTeardown(Group);
        if (Status != RPC_S_OK)
        {
            LeaveCriticalSection(&Group->Lock);
            return Status;
        }
    }

    Group->State = IfGroupClosed;
    Group->Signature = 0;
    LeaveCriticalSection(&Group->Lock);

    /* Wait out a running idle callback before freeing what it uses */
    if (Group->IdleTimer)
        DeleteTimerQueueTimer(NULL, Group->IdleTimer, INVALID_HANDLE_VALUE);

    IfGroupFree(Group);
    return RPC_S_OK;
}

/***********************************************************************
 *             RpcServerInterfaceGroupInqBindings (RPCRT4.@)
 */
RPC_STATUS
RPC_ENTRY
RpcServerInterfaceGroupInqBindings(
    _In_ RPC_INTERFACE_GROUP IfGroup,
    _Outptr_ RPC_BINDING_VECTOR **BindingVector)
{
    PIFGROUP Group = IfGroupFromHandle(IfGroup);
    RPC_STATUS Status;

    TRACE("(%p %p)\n", IfGroup, BindingVector);

    if (!Group || !BindingVector)
        return RPC_S_INVALID_ARG;

    EnterCriticalSection(&Group->Lock);
    Status = IfGroupQueryBindings(Group, FALSE, BindingVector);
    LeaveCriticalSection(&Group->Lock);

    return Status;
}
