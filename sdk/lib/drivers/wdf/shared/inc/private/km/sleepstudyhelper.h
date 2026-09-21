/*
 * PROJECT:     Kernel Mode Device Framework
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Sleep Study helper library, for a kernel with no Sleep Study
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * The framework reaches these only after the DRIPS WNF check passes, which
 * never happens here. Initialization failing keeps them unreachable anyway.
 */

#pragma once

/* ReactOS's own tag, only seen if a Sleep Study context is ever allocated. */
#define SLEEPSTUDY_POOL_TAG         'tSlS'
#define FRIENDLY_NAME_MAX_LENGTH    256

typedef struct _SS_LIBRARY_CONTEXT *SS_LIBRARY;
typedef struct _SS_COMPONENT_CONTEXT *SS_COMPONENT;

typedef enum _SSH_OBJECT_TYPE
{
    SSH_PDO,
    SSH_FDO
} SSH_OBJECT_TYPE;

__inline
NTSTATUS
SleepstudyHelper_Initialize(
    _Out_ SS_LIBRARY *Library,
    _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);

    *Library = NULL;
    return STATUS_NOT_SUPPORTED;
}

__inline
VOID
SleepstudyHelper_Uninitialize(
    _In_ SS_LIBRARY Library)
{
    UNREFERENCED_PARAMETER(Library);
}

__inline
VOID
SleepstudyHelper_GenerateGuid(
    _In_ SSH_OBJECT_TYPE ObjectType,
    _In_ ULONG64 Object,
    _Out_ GUID *Guid)
{
    UNREFERENCED_PARAMETER(ObjectType);
    UNREFERENCED_PARAMETER(Object);

    RtlZeroMemory(Guid, sizeof(*Guid));
}

__inline
NTSTATUS
SleepstudyHelper_GetPdoFriendlyName(
    _In_ PDEVICE_OBJECT Pdo,
    _Inout_ PUNICODE_STRING FriendlyName)
{
    UNREFERENCED_PARAMETER(Pdo);
    UNREFERENCED_PARAMETER(FriendlyName);

    return STATUS_NOT_SUPPORTED;
}

__inline
NTSTATUS
SleepstudyHelper_RegisterComponentEx(
    _In_ SS_LIBRARY Library,
    _In_ GUID ParentGuid,
    _In_ GUID ComponentGuid,
    _In_ PUNICODE_STRING FriendlyName,
    _Out_ SS_COMPONENT *Component)
{
    UNREFERENCED_PARAMETER(Library);
    UNREFERENCED_PARAMETER(ParentGuid);
    UNREFERENCED_PARAMETER(ComponentGuid);
    UNREFERENCED_PARAMETER(FriendlyName);

    *Component = NULL;
    return STATUS_NOT_SUPPORTED;
}

__inline
VOID
SleepstudyHelper_UnregisterComponent(
    _In_ SS_COMPONENT Component)
{
    UNREFERENCED_PARAMETER(Component);
}

__inline
VOID
SleepstudyHelper_AcquireComponentLock(
    _In_ SS_COMPONENT Component,
    _Out_ PKIRQL OldIrql)
{
    UNREFERENCED_PARAMETER(Component);

    *OldIrql = KeGetCurrentIrql();
}

__inline
VOID
SleepstudyHelper_ReleaseComponentLock(
    _In_ SS_COMPONENT Component,
    _In_ KIRQL OldIrql)
{
    UNREFERENCED_PARAMETER(Component);
    UNREFERENCED_PARAMETER(OldIrql);
}

__inline
VOID
SleepstudyHelper_ComponentActive(
    _In_ SS_COMPONENT Component)
{
    UNREFERENCED_PARAMETER(Component);
}

__inline
VOID
SleepstudyHelper_ComponentActiveLocked(
    _In_ SS_COMPONENT Component)
{
    UNREFERENCED_PARAMETER(Component);
}

__inline
VOID
SleepstudyHelper_ComponentInactive(
    _In_ SS_COMPONENT Component)
{
    UNREFERENCED_PARAMETER(Component);
}

__inline
VOID
SleepstudyHelper_ResetComponentsStartTime(
    _In_ SS_COMPONENT Component)
{
    UNREFERENCED_PARAMETER(Component);
}
