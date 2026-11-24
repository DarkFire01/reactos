/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     WMI data block access through the kernel WMI device
 */

#include <advapi32.h>
#include <winioctl.h>
#include <wmium.h>
#include <wmiioctl.h>

/* WNODE_ALL_DATA with room for one OFFSETINSTANCEDATAANDLENGTH, the least the kernel accepts */
#define WMIP_MIN_ALL_DATA_SIZE \
    ((FIELD_OFFSET(WNODE_ALL_DATA, OffsetInstanceDataAndLength) + \
      sizeof(OFFSETINSTANCEDATAANDLENGTH) + 7) & ~7)

#define WMIP_ALIGN8(Value) (((Value) + 7) & ~7)

static HANDLE WmipKernelDevice;

static
ULONG
WmipGetKernelDevice(
    _Out_ PHANDLE Device)
{
    HANDLE Handle;

    *Device = WmipKernelDevice;
    if (*Device)
        return ERROR_SUCCESS;

    Handle = CreateFileW(L"\\\\.\\WMIDataDevice",
                         GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL,
                         OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                         NULL);
    /* Queries only need read access, which is all some callers are granted */
    if ((Handle == INVALID_HANDLE_VALUE) && (GetLastError() == ERROR_ACCESS_DENIED))
    {
        Handle = CreateFileW(L"\\\\.\\WMIDataDevice",
                             GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL,
                             OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                             NULL);
    }

    if (Handle == INVALID_HANDLE_VALUE)
        return GetLastError();

    /* Another thread may have won the race, keep its handle */
    if (InterlockedCompareExchangePointer(&WmipKernelDevice, Handle, NULL) != NULL)
        CloseHandle(Handle);

    *Device = WmipKernelDevice;
    return ERROR_SUCCESS;
}

/**
 * @brief Sends one request to the WMI device and waits for it, returning a Win32 error code.
 */
static
ULONG
WmipSendKernelRequest(
    _In_ ULONG IoControlCode,
    _In_reads_bytes_(InputSize) PVOID Input,
    _In_ ULONG InputSize,
    _Out_writes_bytes_(OutputSize) PVOID Output,
    _In_ ULONG OutputSize,
    _Out_ PULONG ReturnSize)
{
    OVERLAPPED Overlapped;
    DWORD Returned = 0;
    HANDLE Device;
    ULONG Error;

    *ReturnSize = 0;

    Error = WmipGetKernelDevice(&Device);
    if (Error != ERROR_SUCCESS)
        return Error;

    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Overlapped.hEvent)
        return GetLastError();

    Error = ERROR_SUCCESS;
    if (!DeviceIoControl(Device,
                         IoControlCode,
                         Input,
                         InputSize,
                         Output,
                         OutputSize,
                         &Returned,
                         &Overlapped))
    {
        Error = GetLastError();
        if (Error == ERROR_IO_PENDING)
        {
            Error = ERROR_SUCCESS;
            if (!GetOverlappedResult(Device, &Overlapped, &Returned, TRUE))
                Error = GetLastError();
        }
    }

    CloseHandle(Overlapped.hEvent);
    *ReturnSize = Returned;
    return Error;
}

/*
 * @implemented
 */
ULONG
WINAPI
WmiOpenBlock(
    _In_ GUID *Guid,
    _In_ ULONG DesiredAccess,
    _Out_ WMIHANDLE *DataBlockHandle)
{
    WCHAR ObjectNameBuffer[WMI_GUID_OBJECT_NAME_LENGTH + 1];
    OBJECT_ATTRIBUTES ObjectAttributes;
    WMIOPENGUIDBLOCK OpenBlock;
    UNICODE_STRING ObjectName;
    ULONG IoControlCode, ReturnSize, Error;

    if (!Guid || !DataBlockHandle)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return ERROR_INVALID_PARAMETER;
    }

    *DataBlockHandle = NULL;

    /* Event consumers may open GUIDs nobody serves yet, queries need a provider */
    if (DesiredAccess & WMIGUID_NOTIFICATION)
        IoControlCode = IOCTL_WMI_OPEN_GUID_FOR_EVENTS;
    else if (DesiredAccess & WRITE_DAC)
        IoControlCode = IOCTL_WMI_OPEN_GUID;
    else
        IoControlCode = IOCTL_WMI_OPEN_GUID_FOR_QUERYSET;

    _snwprintf(ObjectNameBuffer,
               RTL_NUMBER_OF(ObjectNameBuffer),
               L"\\WmiGuid\\%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
               Guid->Data1,
               Guid->Data2,
               Guid->Data3,
               Guid->Data4[0],
               Guid->Data4[1],
               Guid->Data4[2],
               Guid->Data4[3],
               Guid->Data4[4],
               Guid->Data4[5],
               Guid->Data4[6],
               Guid->Data4[7]);
    ObjectNameBuffer[WMI_GUID_OBJECT_NAME_LENGTH] = UNICODE_NULL;

    RtlInitUnicodeString(&ObjectName, ObjectNameBuffer);
    InitializeObjectAttributes(&ObjectAttributes, &ObjectName, 0, NULL, NULL);

    ZeroMemory(&OpenBlock, sizeof(OpenBlock));
    OpenBlock.ObjectAttributes = &ObjectAttributes;
    OpenBlock.DesiredAccess = DesiredAccess;

    Error = WmipSendKernelRequest(IoControlCode,
                                  &OpenBlock,
                                  sizeof(OpenBlock),
                                  &OpenBlock,
                                  sizeof(OpenBlock),
                                  &ReturnSize);
    if (Error == ERROR_SUCCESS)
        *DataBlockHandle = OpenBlock.Handle;

    SetLastError(Error);
    return Error;
}

/*
 * @implemented
 */
ULONG
WINAPI
WmiCloseBlock(
    _In_ WMIHANDLE DataBlockHandle)
{
    ULONG Error = ERROR_SUCCESS;

    if (!CloseHandle(DataBlockHandle))
        Error = GetLastError();

    SetLastError(Error);
    return Error;
}

/*
 * @implemented
 */
ULONG
WINAPI
WmiQueryAllDataW(
    _In_ WMIHANDLE DataBlockHandle,
    _Inout_ ULONG *BufferSize,
    _Out_writes_bytes_opt_(*BufferSize) LPVOID Buffer)
{
    ULONG64 Local[WMIP_MIN_ALL_DATA_SIZE / sizeof(ULONG64)];
    PWNODE_ALL_DATA Wnode;
    ULONG Size, ReturnSize, Error;

    if (!BufferSize)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return ERROR_INVALID_PARAMETER;
    }

    /* Without room for a WNODE_ALL_DATA only the needed size can come back */
    if (!Buffer || (*BufferSize < WMIP_MIN_ALL_DATA_SIZE))
    {
        Wnode = (PWNODE_ALL_DATA)Local;
        Size = sizeof(Local);
    }
    else
    {
        Wnode = Buffer;
        Size = *BufferSize;
    }

    ZeroMemory(Wnode, sizeof(WNODE_HEADER));
    Wnode->WnodeHeader.BufferSize = sizeof(WNODE_HEADER);
    Wnode->WnodeHeader.Flags = WNODE_FLAG_ALL_DATA;
    Wnode->WnodeHeader.KernelHandle = DataBlockHandle;

    Error = WmipSendKernelRequest(IOCTL_WMI_QUERY_ALL_DATA,
                                  Wnode,
                                  sizeof(WNODE_HEADER),
                                  Wnode,
                                  Size,
                                  &ReturnSize);
    if (Error == ERROR_SUCCESS)
    {
        if (Wnode->WnodeHeader.Flags & WNODE_FLAG_TOO_SMALL)
        {
            *BufferSize = ((PWNODE_TOO_SMALL)Wnode)->SizeNeeded;
            Error = ERROR_INSUFFICIENT_BUFFER;
        }
        else
        {
            *BufferSize = ReturnSize;
            if ((PVOID)Wnode != Buffer)
                Error = ERROR_INSUFFICIENT_BUFFER;
        }
    }

    SetLastError(Error);
    return Error;
}

/* Rewrites one counted instance name from UTF-16 to the ANSI code page where it lies. */
static
VOID
WmipCountedNameToAnsi(
    _Inout_ PUSHORT CountedName)
{
    ULONG Bytes = CountedName[0];
    ULONG AnsiBytes = 0;
    PCHAR Ansi;

    if (Bytes == 0)
        return;

    Ansi = HeapAlloc(GetProcessHeap(), 0, Bytes);
    if (!Ansi)
        return;

    RtlUnicodeToMultiByteN(Ansi, Bytes, &AnsiBytes, (PWCH)(CountedName + 1), Bytes & ~1);
    CopyMemory(CountedName + 1, Ansi, AnsiBytes);
    CountedName[0] = (USHORT)AnsiBytes;

    HeapFree(GetProcessHeap(), 0, Ansi);
}

/*
 * @implemented
 */
ULONG
WINAPI
WmiQueryAllDataA(
    _In_ WMIHANDLE DataBlockHandle,
    _Inout_ ULONG *BufferSize,
    _Out_writes_bytes_opt_(*BufferSize) LPVOID Buffer)
{
    PWNODE_ALL_DATA Wnode = Buffer;
    PULONG NameOffsets;
    ULONG Error, Index;

    Error = WmiQueryAllDataW(DataBlockHandle, BufferSize, Buffer);
    if (Error != ERROR_SUCCESS)
        return Error;

    /* Every provider's WNODE in the chain carries its own name table */
    for (;;)
    {
        if (Wnode->OffsetInstanceNameOffsets && Wnode->InstanceCount)
        {
            NameOffsets = (PULONG)((PUCHAR)Wnode + Wnode->OffsetInstanceNameOffsets);
            for (Index = 0; Index < Wnode->InstanceCount; Index++)
                WmipCountedNameToAnsi((PUSHORT)((PUCHAR)Wnode + NameOffsets[Index]));
            Wnode->WnodeHeader.Flags |= WNODE_FLAG_ANSI_INSTANCENAMES;
        }

        if (!Wnode->WnodeHeader.Linkage)
            break;
        Wnode = (PWNODE_ALL_DATA)((PUCHAR)Wnode + Wnode->WnodeHeader.Linkage);
    }

    SetLastError(ERROR_SUCCESS);
    return ERROR_SUCCESS;
}

/*
 * @implemented
 */
ULONG
WINAPI
WmiQuerySingleInstanceW(
    _In_ WMIHANDLE DataBlockHandle,
    _In_ LPCWSTR InstanceName,
    _Inout_ ULONG *BufferSize,
    _Out_writes_bytes_opt_(*BufferSize) LPVOID Buffer)
{
    PWNODE_SINGLE_INSTANCE Wnode;
    ULONG WnodeSize, Size, ReturnSize, Error;
    SIZE_T NameLength;

    if (!InstanceName || !BufferSize)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return ERROR_INVALID_PARAMETER;
    }

    NameLength = wcslen(InstanceName) * sizeof(WCHAR);
    if (NameLength > MAXUSHORT)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return ERROR_INVALID_PARAMETER;
    }

    /* Header and counted name, the data starts at the next eight byte boundary */
    WnodeSize = (ULONG)WMIP_ALIGN8(FIELD_OFFSET(WNODE_SINGLE_INSTANCE, VariableData) +
                                   sizeof(USHORT) + NameLength);

    if (Buffer && (*BufferSize >= WnodeSize))
    {
        Wnode = Buffer;
        Size = *BufferSize;
    }
    else
    {
        Wnode = HeapAlloc(GetProcessHeap(), 0, WnodeSize);
        if (!Wnode)
        {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        Size = WnodeSize;
    }

    ZeroMemory(Wnode, FIELD_OFFSET(WNODE_SINGLE_INSTANCE, VariableData));
    Wnode->WnodeHeader.BufferSize = WnodeSize;
    Wnode->WnodeHeader.Flags = WNODE_FLAG_SINGLE_INSTANCE;
    Wnode->WnodeHeader.KernelHandle = DataBlockHandle;
    Wnode->OffsetInstanceName = FIELD_OFFSET(WNODE_SINGLE_INSTANCE, VariableData);
    Wnode->DataBlockOffset = WnodeSize;
    *(PUSHORT)Wnode->VariableData = (USHORT)NameLength;
    CopyMemory(Wnode->VariableData + sizeof(USHORT), InstanceName, NameLength);

    Error = WmipSendKernelRequest(IOCTL_WMI_QUERY_SINGLE_INSTANCE,
                                  Wnode,
                                  WnodeSize,
                                  Wnode,
                                  Size,
                                  &ReturnSize);
    if (Error == ERROR_SUCCESS)
    {
        if (Wnode->WnodeHeader.Flags & WNODE_FLAG_TOO_SMALL)
        {
            *BufferSize = ((PWNODE_TOO_SMALL)Wnode)->SizeNeeded;
            Error = ERROR_INSUFFICIENT_BUFFER;
        }
        else
        {
            *BufferSize = ReturnSize;
            if ((PVOID)Wnode != Buffer)
                Error = ERROR_INSUFFICIENT_BUFFER;
        }
    }

    if ((PVOID)Wnode != Buffer)
        HeapFree(GetProcessHeap(), 0, Wnode);

    SetLastError(Error);
    return Error;
}

/*
 * @implemented
 */
ULONG
WINAPI
WmiQuerySingleInstanceA(
    _In_ WMIHANDLE DataBlockHandle,
    _In_ LPCSTR InstanceName,
    _Inout_ ULONG *BufferSize,
    _Out_writes_bytes_opt_(*BufferSize) LPVOID Buffer)
{
    PWNODE_SINGLE_INSTANCE Wnode = Buffer;
    UNICODE_STRING Name;
    ULONG Error;

    if (!InstanceName)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return ERROR_INVALID_PARAMETER;
    }

    if (!RtlCreateUnicodeStringFromAsciiz(&Name, InstanceName))
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    Error = WmiQuerySingleInstanceW(DataBlockHandle, Name.Buffer, BufferSize, Buffer);
    RtlFreeUnicodeString(&Name);

    if (Error == ERROR_SUCCESS)
    {
        WmipCountedNameToAnsi((PUSHORT)((PUCHAR)Wnode + Wnode->OffsetInstanceName));
        Wnode->WnodeHeader.Flags |= WNODE_FLAG_ANSI_INSTANCENAMES;
        SetLastError(ERROR_SUCCESS);
    }

    return Error;
}
