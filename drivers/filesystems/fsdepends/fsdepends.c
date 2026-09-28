/*
 * PROJECT:     ReactOS File System Dependency Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     What a virtual disk is made of, so nothing takes it away
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * A virtual disk is a file on some other volume. Surfacing one makes that file
 * something the system is running on, so the volume holding it must not be
 * dismounted, and the file must not be handed to a second virtual disk. A
 * driver that surfaces one says so here and gets back a registration; anything
 * that would pull the ground out from under it asks here first.
 *
 * What is kept is the registration and what it is made of. Reaching up from a
 * host file to dismount whatever was mounted on the disk it backs is not done
 * yet, so a dismount here only takes the registration down.
 */

/* INCLUDES *******************************************************************/

#include <ntifs.h>
#include <ntddk.h>

#define NDEBUG
#include <debug.h>

/* DEFINITIONS ****************************************************************/

#define TAG_FSD_DEPENDENCY 'DpsF'
#define TAG_FSD_NAME       'NpsF'

#define FSD_DEPENDENCY_SIGNATURE 'FsDp'

/* Set on a registration that is being taken down and may not be used again */
#define FSD_DEPENDENCY_TEARING_DOWN 0x00000001

/*
 * What a caller of DependentFSCheckStoragePrivilege is asking to do with the
 * file. Anything beyond reading it wants the privilege that says the caller
 * may speak for a volume.
 */
#define FSD_ACCESS_BEYOND_READ 0x00070000

/* What DependentFSGetVirtualStoragePolicies is asked about */
#define FSD_POLICY_DENIED    0x00000004
#define FSD_POLICY_LOCAL     0x00000010
#define FSD_POLICY_DELEGATED 0x00000020

typedef struct _FSD_DEPENDENCY
{
    ULONG Signature;
    LIST_ENTRY Link;
    ULONG ReferenceCount;
    ULONG Flags;
    PFILE_OBJECT HostFile;
    UNICODE_STRING DeviceName;
} FSD_DEPENDENCY, *PFSD_DEPENDENCY;

NTSTATUS
NTAPI
DependentFSRegisterDeviceName(
    _Inout_ PVOID Registration,
    _In_ PCUNICODE_STRING DeviceName);

/* GLOBALS ********************************************************************/

static PDEVICE_OBJECT FsdDeviceObject = NULL;
static ERESOURCE FsdLock;
static LIST_ENTRY FsdDependencyList;
static BOOLEAN FsdUnloading = FALSE;

/* PRIVATE FUNCTIONS **********************************************************/

/**
 * @brief
 * Says whether the driver is in a state to answer at all.
 */
static
NTSTATUS
FsdCheckReady(VOID)
{
    if (FsdDeviceObject == NULL)
        return STATUS_DEVICE_DOES_NOT_EXIST;

    if (FsdUnloading)
        return STATUS_TOO_LATE;

    return STATUS_SUCCESS;
}

static
VOID
FsdAcquireLock(VOID)
{
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&FsdLock, TRUE);
}

static
VOID
FsdReleaseLock(VOID)
{
    ExReleaseResourceLite(&FsdLock);
    KeLeaveCriticalRegion();
}

/**
 * @brief
 * Checks that what a caller handed back is a registration this driver made and
 * has not already taken down.
 */
static
BOOLEAN
FsdIsLiveDependency(
    _In_opt_ PFSD_DEPENDENCY Dependency)
{
    if (Dependency == NULL)
        return FALSE;

    if (Dependency->Signature != FSD_DEPENDENCY_SIGNATURE)
        return FALSE;

    if (Dependency->Flags & FSD_DEPENDENCY_TEARING_DOWN)
        return FALSE;

    return Dependency->ReferenceCount != 0;
}

/**
 * @brief
 * Drops one hold on a registration, taking it off the list and freeing it when
 * the last one goes.
 *
 * @remarks
 * The caller holds the lock.
 */
static
VOID
FsdDereferenceLocked(
    _Inout_ PFSD_DEPENDENCY Dependency)
{
    if (--Dependency->ReferenceCount != 0)
        return;

    RemoveEntryList(&Dependency->Link);

    if (Dependency->HostFile != NULL)
        ObDereferenceObject(Dependency->HostFile);

    if (Dependency->DeviceName.Buffer != NULL)
        ExFreePoolWithTag(Dependency->DeviceName.Buffer, TAG_FSD_NAME);

    Dependency->Signature = 0;
    ExFreePoolWithTag(Dependency, TAG_FSD_DEPENDENCY);
}

/**
 * @brief
 * Says whether a file is already backing a virtual disk.
 *
 * @remarks
 * The caller holds the lock.
 */
static
BOOLEAN
FsdIsFileAlreadyHosting(
    _In_ PFILE_OBJECT HostFile)
{
    PLIST_ENTRY Entry;
    PFSD_DEPENDENCY Dependency;

    for (Entry = FsdDependencyList.Flink;
         Entry != &FsdDependencyList;
         Entry = Entry->Flink)
    {
        Dependency = CONTAINING_RECORD(Entry, FSD_DEPENDENCY, Link);

        if (Dependency->HostFile == HostFile)
            return TRUE;
    }

    return FALSE;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Takes note that a driver is about to surface a virtual disk made out of a
 * file, and hands back what to name it by afterwards.
 *
 * @param HostFile
 * The file the disk is made of. It is held for as long as the registration is.
 *
 * @param DeviceName
 * What the disk will be surfaced as, if the caller knows it yet.
 *
 * @param Registration
 * Takes what every later call names the disk by.
 */
NTSTATUS
NTAPI
DependentFSRegister(
    _In_opt_ PVOID Reserved1,
    _In_opt_ PVOID Reserved2,
    _In_ PFILE_OBJECT HostFile,
    _In_opt_ PCUNICODE_STRING DeviceName,
    _In_ ULONG Reserved3,
    _In_opt_ PVOID Reserved4,
    _In_ ULONG Reserved5,
    _In_ ULONG Reserved6,
    _In_opt_ PVOID Reserved7,
    _In_opt_ PVOID Reserved8,
    _In_ ULONG Reserved9,
    _Outptr_ PVOID *Registration)
{
    PFSD_DEPENDENCY Dependency;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Reserved1);
    UNREFERENCED_PARAMETER(Reserved2);
    UNREFERENCED_PARAMETER(Reserved3);
    UNREFERENCED_PARAMETER(Reserved4);
    UNREFERENCED_PARAMETER(Reserved5);
    UNREFERENCED_PARAMETER(Reserved6);
    UNREFERENCED_PARAMETER(Reserved7);
    UNREFERENCED_PARAMETER(Reserved8);
    UNREFERENCED_PARAMETER(Reserved9);

    if (Registration == NULL)
        return STATUS_INVALID_PARAMETER;

    Status = FsdCheckReady();
    if (!NT_SUCCESS(Status))
        return Status;

    if (HostFile == NULL)
        return STATUS_INVALID_PARAMETER;

    Dependency = ExAllocatePoolZero(NonPagedPool,
                                    sizeof(*Dependency),
                                    TAG_FSD_DEPENDENCY);
    if (Dependency == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Dependency->Signature = FSD_DEPENDENCY_SIGNATURE;
    Dependency->ReferenceCount = 1;
    Dependency->HostFile = HostFile;

    FsdAcquireLock();

    /* One file makes one disk, or the two would write over each other */
    if (FsdIsFileAlreadyHosting(HostFile))
    {
        FsdReleaseLock();
        ExFreePoolWithTag(Dependency, TAG_FSD_DEPENDENCY);
        return STATUS_SHARING_VIOLATION;
    }

    ObReferenceObject(HostFile);
    InsertTailList(&FsdDependencyList, &Dependency->Link);

    FsdReleaseLock();

    *Registration = Dependency;

    if (DeviceName != NULL && DeviceName->Length != 0)
        DependentFSRegisterDeviceName(Dependency, DeviceName);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Says the virtual disk is gone, so what it was made of is ordinary again.
 */
NTSTATUS
NTAPI
DependentFSUnregister(
    _In_opt_ PVOID Reserved1,
    _In_opt_ PVOID Reserved2,
    _In_ BOOLEAN Reserved3,
    _In_opt_ PVOID Reserved4,
    _In_opt_ PVOID Registration)
{
    PFSD_DEPENDENCY Dependency = Registration;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Reserved1);
    UNREFERENCED_PARAMETER(Reserved2);
    UNREFERENCED_PARAMETER(Reserved3);
    UNREFERENCED_PARAMETER(Reserved4);

    Status = FsdCheckReady();
    if (!NT_SUCCESS(Status))
        return Status;

    FsdAcquireLock();

    if (!FsdIsLiveDependency(Dependency))
    {
        FsdReleaseLock();
        return STATUS_INVALID_PARAMETER;
    }

    /* Nothing may take a fresh hold on it from here on */
    Dependency->Flags |= FSD_DEPENDENCY_TEARING_DOWN;
    FsdDereferenceLocked(Dependency);

    FsdReleaseLock();

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Says what the virtual disk was surfaced as, once the caller knows.
 *
 * @return
 * STATUS_INVALID_PARAMETER if it was already named something else, since a
 * disk that answers to two names cannot be found by either.
 */
NTSTATUS
NTAPI
DependentFSRegisterDeviceName(
    _Inout_ PVOID Registration,
    _In_ PCUNICODE_STRING DeviceName)
{
    PFSD_DEPENDENCY Dependency = Registration;
    PWSTR Buffer;

    if (Dependency == NULL || DeviceName == NULL)
        return STATUS_INVALID_PARAMETER;

    if (Dependency->DeviceName.Buffer != NULL)
    {
        if (RtlCompareUnicodeString(DeviceName,
                                    &Dependency->DeviceName,
                                    FALSE) != 0)
        {
            return STATUS_INVALID_PARAMETER;
        }

        return STATUS_SUCCESS;
    }

    Buffer = ExAllocatePoolWithTag(PagedPool, DeviceName->Length, TAG_FSD_NAME);
    if (Buffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyMemory(Buffer, DeviceName->Buffer, DeviceName->Length);

    Dependency->DeviceName.Buffer = Buffer;
    Dependency->DeviceName.Length = DeviceName->Length;
    Dependency->DeviceName.MaximumLength = DeviceName->Length;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Takes a hold on a registration, so that what it stands for outlives whoever
 * surfaced it.
 */
NTSTATUS
NTAPI
DependentFSReferenceDependency(
    _Inout_ PVOID Registration)
{
    PFSD_DEPENDENCY Dependency = Registration;
    NTSTATUS Status;

    Status = FsdCheckReady();
    if (!NT_SUCCESS(Status))
        return Status;

    FsdAcquireLock();

    if (!FsdIsLiveDependency(Dependency))
    {
        FsdReleaseLock();
        return STATUS_INVALID_PARAMETER;
    }

    Dependency->ReferenceCount++;

    FsdReleaseLock();

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Gives back a hold taken by DependentFSReferenceDependency.
 */
NTSTATUS
NTAPI
DependentFSDereferenceDependency(
    _Inout_ PVOID Registration)
{
    PFSD_DEPENDENCY Dependency = Registration;
    NTSTATUS Status;

    Status = FsdCheckReady();
    if (!NT_SUCCESS(Status))
        return Status;

    FsdAcquireLock();

    if (Dependency == NULL ||
        Dependency->Signature != FSD_DEPENDENCY_SIGNATURE ||
        Dependency->ReferenceCount == 0)
    {
        FsdReleaseLock();
        return STATUS_INVALID_PARAMETER;
    }

    FsdDereferenceLocked(Dependency);

    FsdReleaseLock();

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Says whether a file may be made into a virtual disk.
 *
 * @param NestingLevel
 * Takes how many virtual disks deep the file already sits.
 *
 * @remarks
 * Nothing here surfaces a disk on top of another one yet, so a file is always
 * at the bottom and the answer is the first level.
 */
NTSTATUS
NTAPI
DependentFSCheckFileHandle(
    _In_opt_ PVOID Reserved1,
    _In_opt_ PVOID Reserved2,
    _In_ ULONG Reserved3,
    _In_opt_ PVOID Reserved4,
    _In_opt_ PVOID Reserved5,
    _In_opt_ PVOID Reserved6,
    _Out_ PULONG NestingLevel)
{
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Reserved1);
    UNREFERENCED_PARAMETER(Reserved2);
    UNREFERENCED_PARAMETER(Reserved3);
    UNREFERENCED_PARAMETER(Reserved4);
    UNREFERENCED_PARAMETER(Reserved5);
    UNREFERENCED_PARAMETER(Reserved6);

    if (NestingLevel == NULL)
        return STATUS_INVALID_PARAMETER;

    /* A file is never itself on a virtual disk here, so it is the first level */
    *NestingLevel = 1;

    Status = FsdCheckReady();
    if (!NT_SUCCESS(Status))
        return Status;

    if (*NestingLevel > FsRtlQueryMaximumVirtualDiskNestingLevel())
        return STATUS_VHD_INVALID_STATE;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Says whether the caller may do what it is asking to a virtual disk file.
 *
 * @remarks
 * Reading one is ordinary. Anything more speaks for the volume the disk will
 * become, which is what the privilege is for.
 */
NTSTATUS
NTAPI
DependentFSCheckStoragePrivilege(
    _In_opt_ PSECURITY_SUBJECT_CONTEXT SubjectContext,
    _In_opt_ PULONG Reserved,
    _In_ ULONG DesiredAccess)
{
    PRIVILEGE_SET Privileges;

    UNREFERENCED_PARAMETER(Reserved);

    if (!(DesiredAccess & FSD_ACCESS_BEYOND_READ))
        return STATUS_SUCCESS;

    if (SubjectContext == NULL)
        return STATUS_PRIVILEGE_NOT_HELD;

    Privileges.PrivilegeCount = 1;
    Privileges.Control = PRIVILEGE_SET_ALL_NECESSARY;
    Privileges.Privilege[0].Luid = RtlConvertLongToLuid(SE_MANAGE_VOLUME_PRIVILEGE);
    Privileges.Privilege[0].Attributes = 0;

    if (!SePrivilegeCheck(&Privileges, SubjectContext, UserMode))
        return STATUS_PRIVILEGE_NOT_HELD;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Says whether a virtual disk described this way may be surfaced.
 */
NTSTATUS
NTAPI
DependentFSGetVirtualStoragePolicies(
    _In_ ULONG Flags,
    _In_opt_ PVOID Reserved1,
    _In_opt_ PVOID Reserved2,
    _In_opt_ PVOID Reserved3,
    _Out_ PULONG Allowed)
{
    UNREFERENCED_PARAMETER(Reserved1);
    UNREFERENCED_PARAMETER(Reserved2);
    UNREFERENCED_PARAMETER(Reserved3);

    if (Allowed == NULL)
        return STATUS_INVALID_PARAMETER;

    /* Nothing here says no on its own, so only the request itself can */
    if (Flags & FSD_POLICY_DENIED)
        *Allowed = FALSE;
    else if (Flags & (FSD_POLICY_LOCAL | FSD_POLICY_DELEGATED))
        *Allowed = TRUE;
    else
        *Allowed = FALSE;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Takes down what is mounted on a virtual disk because the system is going
 * away underneath it.
 */
NTSTATUS
NTAPI
DependentFSDismountForSystemEvent(
    _In_ ULONG Reserved1,
    _In_ ULONG Reserved2,
    _In_opt_ PVOID Reserved3,
    _In_opt_ PVOID Reserved4)
{
    UNREFERENCED_PARAMETER(Reserved1);
    UNREFERENCED_PARAMETER(Reserved2);
    UNREFERENCED_PARAMETER(Reserved3);
    UNREFERENCED_PARAMETER(Reserved4);

    /* Nothing is ever mounted on one of these here, so nothing comes down */
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Takes down what is mounted on a virtual disk that is about to stop being a
 * disk at all.
 */
NTSTATUS
NTAPI
DependentFSDismountForUnsurface(
    _In_opt_ PVOID Reserved1,
    _In_opt_ PVOID Reserved2,
    _In_opt_ PVOID Reserved3,
    _In_opt_ PVOID Registration)
{
    PFSD_DEPENDENCY Dependency = Registration;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Reserved1);
    UNREFERENCED_PARAMETER(Reserved2);
    UNREFERENCED_PARAMETER(Reserved3);

    Status = FsdCheckReady();
    if (!NT_SUCCESS(Status))
        return Status;

    FsdAcquireLock();

    if (!FsdIsLiveDependency(Dependency))
        Status = STATUS_INVALID_PARAMETER;

    FsdReleaseLock();

    return Status;
}

/* DRIVER *********************************************************************/

static
_Function_class_(DRIVER_UNLOAD)
VOID
NTAPI
FsdUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);

    FsdUnloading = TRUE;

    if (FsdDeviceObject != NULL)
    {
        IoDeleteDevice(FsdDeviceObject);
        FsdDeviceObject = NULL;
    }

    ExDeleteResourceLite(&FsdLock);
}

static
_Function_class_(DRIVER_DISPATCH)
NTSTATUS
NTAPI
FsdDispatch(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;
}

CODE_SEG("INIT")
NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING DeviceName = RTL_CONSTANT_STRING(L"\\Device\\FsDepends");
    NTSTATUS Status;
    ULONG i;

    UNREFERENCED_PARAMETER(RegistryPath);

    InitializeListHead(&FsdDependencyList);

    Status = ExInitializeResourceLite(&FsdLock);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = IoCreateDevice(DriverObject,
                            0,
                            &DeviceName,
                            FILE_DEVICE_UNKNOWN,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &FsdDeviceObject);
    if (!NT_SUCCESS(Status))
    {
        ExDeleteResourceLite(&FsdLock);
        return Status;
    }

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        DriverObject->MajorFunction[i] = FsdDispatch;

    DriverObject->DriverUnload = FsdUnload;

    FsdDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;
}

/* EOF */
