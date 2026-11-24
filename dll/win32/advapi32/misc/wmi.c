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

/* Builds a WNODE for one named instance with DataSize bytes of room eight byte aligned behind the name. */
static
ULONG
WmipAllocateInstanceWnode(
    _In_ ULONG HeaderSize,
    _In_ LPCWSTR InstanceName,
    _In_ ULONG DataSize,
    _Out_ PULONG DataOffset,
    _Out_ PULONG WnodeSize,
    _Outptr_ PWNODE_SINGLE_INSTANCE *OutWnode)
{
    PWNODE_SINGLE_INSTANCE Wnode;
    ULONGLONG Offset, Size;
    SIZE_T NameLength;

    *OutWnode = NULL;

    NameLength = wcslen(InstanceName) * sizeof(WCHAR);
    if (NameLength > MAXUSHORT)
        return ERROR_INVALID_PARAMETER;

    Offset = WMIP_ALIGN8((ULONGLONG)HeaderSize + sizeof(USHORT) + NameLength);
    Size = Offset + DataSize;
    if (Size > MAXULONG)
        return ERROR_INVALID_PARAMETER;

    Wnode = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)Size);
    if (!Wnode)
        return ERROR_NOT_ENOUGH_MEMORY;

    Wnode->OffsetInstanceName = HeaderSize;
    *(PUSHORT)((PUCHAR)Wnode + HeaderSize) = (USHORT)NameLength;
    CopyMemory((PUCHAR)Wnode + HeaderSize + sizeof(USHORT), InstanceName, NameLength);

    *DataOffset = (ULONG)Offset;
    *WnodeSize = (ULONG)Size;
    *OutWnode = Wnode;
    return ERROR_SUCCESS;
}

/*
 * @implemented
 */
ULONG
WINAPI
WmiSetSingleInstanceW(
    _In_ WMIHANDLE DataBlockHandle,
    _In_ LPCWSTR InstanceName,
    _In_ ULONG Reserved,
    _In_ ULONG ValueBufferSize,
    _In_reads_bytes_(ValueBufferSize) PVOID ValueBuffer)
{
    PWNODE_SINGLE_INSTANCE Wnode;
    ULONG DataOffset, WnodeSize, ReturnSize, Error;

    if (!InstanceName || (ValueBufferSize && !ValueBuffer))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return ERROR_INVALID_PARAMETER;
    }

    Error = WmipAllocateInstanceWnode(FIELD_OFFSET(WNODE_SINGLE_INSTANCE, VariableData),
                                      InstanceName,
                                      ValueBufferSize,
                                      &DataOffset,
                                      &WnodeSize,
                                      &Wnode);
    if (Error != ERROR_SUCCESS)
    {
        SetLastError(Error);
        return Error;
    }

    Wnode->WnodeHeader.BufferSize = WnodeSize;
    Wnode->WnodeHeader.Version = Reserved;
    Wnode->WnodeHeader.Flags = WNODE_FLAG_SINGLE_INSTANCE;
    Wnode->WnodeHeader.KernelHandle = DataBlockHandle;
    Wnode->DataBlockOffset = DataOffset;
    Wnode->SizeDataBlock = ValueBufferSize;
    if (ValueBufferSize)
        CopyMemory((PUCHAR)Wnode + DataOffset, ValueBuffer, ValueBufferSize);

    Error = WmipSendKernelRequest(IOCTL_WMI_SET_SINGLE_INSTANCE,
                                  Wnode,
                                  WnodeSize,
                                  NULL,
                                  0,
                                  &ReturnSize);

    HeapFree(GetProcessHeap(), 0, Wnode);
    SetLastError(Error);
    return Error;
}

/*
 * @implemented
 */
ULONG
WINAPI
WmiSetSingleInstanceA(
    _In_ WMIHANDLE DataBlockHandle,
    _In_ LPCSTR InstanceName,
    _In_ ULONG Reserved,
    _In_ ULONG ValueBufferSize,
    _In_reads_bytes_(ValueBufferSize) PVOID ValueBuffer)
{
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

    Error = WmiSetSingleInstanceW(DataBlockHandle, Name.Buffer, Reserved, ValueBufferSize, ValueBuffer);
    RtlFreeUnicodeString(&Name);
    return Error;
}

/*
 * @implemented
 */
ULONG
WINAPI
WmiSetSingleItemW(
    _In_ WMIHANDLE DataBlockHandle,
    _In_ LPCWSTR InstanceName,
    _In_ ULONG DataItemId,
    _In_ ULONG Reserved,
    _In_ ULONG ValueBufferSize,
    _In_reads_bytes_(ValueBufferSize) PVOID ValueBuffer)
{
    PWNODE_SINGLE_ITEM Wnode;
    ULONG DataOffset, WnodeSize, ReturnSize, Error;

    if (!InstanceName || (ValueBufferSize && !ValueBuffer))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return ERROR_INVALID_PARAMETER;
    }

    Error = WmipAllocateInstanceWnode(FIELD_OFFSET(WNODE_SINGLE_ITEM, VariableData),
                                      InstanceName,
                                      ValueBufferSize,
                                      &DataOffset,
                                      &WnodeSize,
                                      (PWNODE_SINGLE_INSTANCE *)&Wnode);
    if (Error != ERROR_SUCCESS)
    {
        SetLastError(Error);
        return Error;
    }

    Wnode->WnodeHeader.BufferSize = WnodeSize;
    Wnode->WnodeHeader.Version = Reserved;
    Wnode->WnodeHeader.Flags = WNODE_FLAG_SINGLE_ITEM;
    Wnode->WnodeHeader.KernelHandle = DataBlockHandle;
    Wnode->ItemId = DataItemId;
    Wnode->DataBlockOffset = DataOffset;
    Wnode->SizeDataItem = ValueBufferSize;
    if (ValueBufferSize)
        CopyMemory((PUCHAR)Wnode + DataOffset, ValueBuffer, ValueBufferSize);

    Error = WmipSendKernelRequest(IOCTL_WMI_SET_SINGLE_ITEM,
                                  Wnode,
                                  WnodeSize,
                                  NULL,
                                  0,
                                  &ReturnSize);

    HeapFree(GetProcessHeap(), 0, Wnode);
    SetLastError(Error);
    return Error;
}

/*
 * @implemented
 */
ULONG
WINAPI
WmiSetSingleItemA(
    _In_ WMIHANDLE DataBlockHandle,
    _In_ LPCSTR InstanceName,
    _In_ ULONG DataItemId,
    _In_ ULONG Reserved,
    _In_ ULONG ValueBufferSize,
    _In_reads_bytes_(ValueBufferSize) PVOID ValueBuffer)
{
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

    Error = WmiSetSingleItemW(DataBlockHandle, Name.Buffer, DataItemId, Reserved, ValueBufferSize, ValueBuffer);
    RtlFreeUnicodeString(&Name);
    return Error;
}

/*
 * @implemented
 */
ULONG
WINAPI
WmiExecuteMethodW(
    _In_ WMIHANDLE MethodDataBlockHandle,
    _In_ LPCWSTR MethodInstanceName,
    _In_ ULONG MethodId,
    _In_ ULONG InputValueBufferSize,
    _In_reads_bytes_opt_(InputValueBufferSize) PVOID InputValueBuffer,
    _Inout_ ULONG *OutputBufferSize,
    _Out_writes_bytes_opt_(*OutputBufferSize) PVOID OutputBuffer)
{
    PWNODE_METHOD_ITEM Wnode;
    ULONG DataOffset, WnodeSize, ReturnSize, OutputRoom, Needed, Error;

    if (!MethodInstanceName || !OutputBufferSize || (InputValueBufferSize && !InputValueBuffer))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return ERROR_INVALID_PARAMETER;
    }

    OutputRoom = OutputBuffer ? *OutputBufferSize : 0;

    /* The output comes back in place of the input, so the WNODE fits the larger one */
    Error = WmipAllocateInstanceWnode(FIELD_OFFSET(WNODE_METHOD_ITEM, VariableData),
                                      MethodInstanceName,
                                      max(InputValueBufferSize, OutputRoom),
                                      &DataOffset,
                                      &WnodeSize,
                                      (PWNODE_SINGLE_INSTANCE *)&Wnode);
    if (Error != ERROR_SUCCESS)
    {
        SetLastError(Error);
        return Error;
    }

    Wnode->WnodeHeader.BufferSize = DataOffset + InputValueBufferSize;
    Wnode->WnodeHeader.Flags = WNODE_FLAG_METHOD_ITEM;
    Wnode->WnodeHeader.KernelHandle = MethodDataBlockHandle;
    Wnode->MethodId = MethodId;
    Wnode->DataBlockOffset = DataOffset;
    Wnode->SizeDataBlock = InputValueBufferSize;
    if (InputValueBufferSize)
        CopyMemory((PUCHAR)Wnode + DataOffset, InputValueBuffer, InputValueBufferSize);

    Error = WmipSendKernelRequest(IOCTL_WMI_EXECUTE_METHOD,
                                  Wnode,
                                  DataOffset + InputValueBufferSize,
                                  Wnode,
                                  WnodeSize,
                                  &ReturnSize);
    if (Error == ERROR_SUCCESS)
    {
        if (Wnode->WnodeHeader.Flags & WNODE_FLAG_TOO_SMALL)
            Needed = ((PWNODE_TOO_SMALL)Wnode)->SizeNeeded - DataOffset;
        else
            Needed = Wnode->SizeDataBlock;

        if ((Wnode->WnodeHeader.Flags & WNODE_FLAG_TOO_SMALL) || (Needed > OutputRoom))
        {
            Error = ERROR_INSUFFICIENT_BUFFER;
        }
        else if (Needed)
        {
            CopyMemory(OutputBuffer, (PUCHAR)Wnode + Wnode->DataBlockOffset, Needed);
        }

        *OutputBufferSize = Needed;
    }

    HeapFree(GetProcessHeap(), 0, Wnode);
    SetLastError(Error);
    return Error;
}

/*
 * @implemented
 */
ULONG
WINAPI
WmiExecuteMethodA(
    _In_ WMIHANDLE MethodDataBlockHandle,
    _In_ LPCSTR MethodInstanceName,
    _In_ ULONG MethodId,
    _In_ ULONG InputValueBufferSize,
    _In_reads_bytes_opt_(InputValueBufferSize) PVOID InputValueBuffer,
    _Inout_ ULONG *OutputBufferSize,
    _Out_writes_bytes_opt_(*OutputBufferSize) PVOID OutputBuffer)
{
    UNICODE_STRING Name;
    ULONG Error;

    if (!MethodInstanceName)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return ERROR_INVALID_PARAMETER;
    }

    if (!RtlCreateUnicodeStringFromAsciiz(&Name, MethodInstanceName))
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    Error = WmiExecuteMethodW(MethodDataBlockHandle,
                              Name.Buffer,
                              MethodId,
                              InputValueBufferSize,
                              InputValueBuffer,
                              OutputBufferSize,
                              OutputBuffer);
    RtlFreeUnicodeString(&Name);
    return Error;
}

/*
 * @implemented
 */
ULONG
WINAPI
WmiQueryGuidInformation(
    _In_ WMIHANDLE GuidHandle,
    _Inout_ PWMIGUIDINFORMATION GuidInfo)
{
    WMI_GUID_INFO_QUERY Query;
    ULONG ReturnSize, Error;

    if (!GuidInfo || (GuidInfo->Size < sizeof(*GuidInfo)))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return ERROR_INVALID_PARAMETER;
    }

    ZeroMemory(&Query, sizeof(Query));
    Query.Handle = GuidHandle;

    Error = WmipSendKernelRequest(IOCTL_WMI_QUERY_GUID_INFO,
                                  &Query,
                                  sizeof(Query),
                                  &Query,
                                  sizeof(Query),
                                  &ReturnSize);
    if (Error == ERROR_SUCCESS)
    {
        GuidInfo->IsExpensive = Query.IsExpensive;
        GuidInfo->IsEventOnly = FALSE;
    }

    SetLastError(Error);
    return Error;
}

/*
 * @implemented
 */
ULONG
WINAPI
WmiEnumerateGuids(
    _Out_writes_to_(*GuidCount, *GuidCount) LPGUID GuidList,
    _Inout_ ULONG *GuidCount)
{
    PWMI_GUID_LIST List;
    ULONG Room, Size, ReturnSize, Index, Error;

    if (!GuidCount || (*GuidCount && !GuidList))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return ERROR_INVALID_PARAMETER;
    }

    Room = *GuidCount;
    if (Room > (MAXULONG - FIELD_OFFSET(WMI_GUID_LIST, Guids)) / sizeof(WMI_GUID_PROPERTIES))
        Room = (MAXULONG - FIELD_OFFSET(WMI_GUID_LIST, Guids)) / sizeof(WMI_GUID_PROPERTIES);

    Size = FIELD_OFFSET(WMI_GUID_LIST, Guids) + Room * sizeof(WMI_GUID_PROPERTIES);
    List = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
    if (!List)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    Error = WmipSendKernelRequest(IOCTL_WMI_ENUMERATE_GUIDS, NULL, 0, List, Size, &ReturnSize);
    if (Error == ERROR_SUCCESS)
    {
        for (Index = 0; (Index < List->ReturnedGuidCount) && (Index < Room); Index++)
            GuidList[Index] = List->Guids[Index].Guid;

        *GuidCount = List->TotalGuidCount;
        if (List->TotalGuidCount > Room)
            Error = ERROR_MORE_DATA;
    }

    HeapFree(GetProcessHeap(), 0, List);
    SetLastError(Error);
    return Error;
}

/*
 * @implemented
 */
VOID
WINAPI
WmiFreeBuffer(
    _In_ PVOID Buffer)
{
    if (Buffer)
        HeapFree(GetProcessHeap(), 0, Buffer);
}

/* Fetches the MOF resource list, growing the buffer to the size the kernel asks for. */
static
ULONG
WmipQueryMofList(
    _Outptr_ PWMI_MOF_LIST *OutList,
    _Out_ PULONG OutSize)
{
    ULONG Size = 0x1000, ReturnSize, Error, Try;
    PWMI_MOF_LIST List;

    *OutList = NULL;
    *OutSize = 0;

    for (Try = 0; Try < 4; Try++)
    {
        List = HeapAlloc(GetProcessHeap(), 0, Size);
        if (!List)
            return ERROR_NOT_ENOUGH_MEMORY;

        Error = WmipSendKernelRequest(IOCTL_WMI_ENUMERATE_MOF_RESOURCES, NULL, 0, List, Size, &ReturnSize);
        if ((Error == ERROR_SUCCESS) && (ReturnSize == sizeof(ULONG)) && (List->Count > Size))
        {
            /* Too small, the count is the size needed */
            Size = List->Count;
            HeapFree(GetProcessHeap(), 0, List);
            continue;
        }

        if (Error != ERROR_SUCCESS)
        {
            HeapFree(GetProcessHeap(), 0, List);
            return Error;
        }

        *OutList = List;
        *OutSize = ReturnSize;
        return ERROR_SUCCESS;
    }

    return ERROR_INSUFFICIENT_BUFFER;
}

/* Reads a terminated string at Offset of the MOF list, NULL when it runs past the end. */
static
PCWSTR
WmipMofListString(
    _In_ PWMI_MOF_LIST List,
    _In_ ULONG ListSize,
    _In_ ULONG Offset)
{
    PCWSTR String, End;

    if ((Offset & 1) || (Offset >= ListSize))
        return NULL;

    String = (PCWSTR)((PUCHAR)List + Offset);
    End = (PCWSTR)((PUCHAR)List + (ListSize & ~1));
    for (; String < End; String++)
    {
        if (!*String)
            return (PCWSTR)((PUCHAR)List + Offset);
    }

    return NULL;
}

/**
 * @brief Finds the image of the driver behind a service key path the kernel reported.
 *
 * Follows the ImagePath value as the service controller does, defaulting to the drivers directory.
 */
static
PWSTR
WmipServiceKeyToImagePath(
    _In_ PCWSTR RegistryPath)
{
    static const WCHAR MachinePrefix[] = L"\\Registry\\Machine\\";
    static const WCHAR SystemRootPrefix[] = L"\\SystemRoot\\";
    static const WCHAR DosPrefix[] = L"\\??\\";
    WCHAR Value[MAX_PATH], Unexpanded[MAX_PATH + 32];
    DWORD Type, Size, Length;
    PCWSTR ServiceName, Path;
    PWSTR Image;
    HKEY Key;
    LONG Error;

    if (_wcsnicmp(RegistryPath, MachinePrefix, RTL_NUMBER_OF(MachinePrefix) - 1) != 0)
        return NULL;

    ServiceName = wcsrchr(RegistryPath, L'\\') + 1;

    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          RegistryPath + RTL_NUMBER_OF(MachinePrefix) - 1,
                          0,
                          KEY_QUERY_VALUE,
                          &Key);
    if (Error != ERROR_SUCCESS)
        return NULL;

    Size = sizeof(Value) - sizeof(WCHAR);
    Error = RegQueryValueExW(Key, L"ImagePath", NULL, &Type, (LPBYTE)Value, &Size);
    RegCloseKey(Key);

    if ((Error == ERROR_SUCCESS) && ((Type == REG_SZ) || (Type == REG_EXPAND_SZ)) && (Size >= sizeof(WCHAR)))
    {
        Value[Size / sizeof(WCHAR)] = UNICODE_NULL;
        Path = Value;

        if (_wcsnicmp(Path, SystemRootPrefix, RTL_NUMBER_OF(SystemRootPrefix) - 1) == 0)
        {
            _snwprintf(Unexpanded, RTL_NUMBER_OF(Unexpanded), L"%%SystemRoot%%\\%ls",
                       Path + RTL_NUMBER_OF(SystemRootPrefix) - 1);
        }
        else if (wcsncmp(Path, DosPrefix, RTL_NUMBER_OF(DosPrefix) - 1) == 0)
        {
            _snwprintf(Unexpanded, RTL_NUMBER_OF(Unexpanded), L"%ls", Path + RTL_NUMBER_OF(DosPrefix) - 1);
        }
        else if (!wcschr(Path, L':') && (Path[0] != L'\\') && (Path[0] != L'%'))
        {
            /* Relative image paths start at the system root */
            _snwprintf(Unexpanded, RTL_NUMBER_OF(Unexpanded), L"%%SystemRoot%%\\%ls", Path);
        }
        else
        {
            _snwprintf(Unexpanded, RTL_NUMBER_OF(Unexpanded), L"%ls", Path);
        }
    }
    else
    {
        _snwprintf(Unexpanded, RTL_NUMBER_OF(Unexpanded), L"%%SystemRoot%%\\System32\\drivers\\%ls.sys", ServiceName);
    }
    Unexpanded[RTL_NUMBER_OF(Unexpanded) - 1] = UNICODE_NULL;

    Length = ExpandEnvironmentStringsW(Unexpanded, NULL, 0);
    if (!Length)
        return NULL;

    Image = HeapAlloc(GetProcessHeap(), 0, Length * sizeof(WCHAR));
    if (Image && !ExpandEnvironmentStringsW(Unexpanded, Image, Length))
    {
        HeapFree(GetProcessHeap(), 0, Image);
        Image = NULL;
    }

    return Image;
}

/*
 * @implemented
 */
ULONG
WINAPI
WmiMofEnumerateResourcesW(
    _In_ MOFHANDLE MofResourceHandle,
    _Out_ ULONG *MofResourceCount,
    _Outptr_result_buffer_(*MofResourceCount) PMOFRESOURCEINFOW *MofResourceInfo)
{
    PMOFRESOURCEINFOW Info = NULL;
    PWSTR *Images = NULL;
    PWMI_MOF_LIST List;
    PCWSTR Path, Name;
    ULONG ListSize, Index, Count = 0, Error;
    SIZE_T Size, ImageSize, NameSize;
    PWCHAR Strings;

    UNREFERENCED_PARAMETER(MofResourceHandle);

    if (!MofResourceCount || !MofResourceInfo)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return ERROR_INVALID_PARAMETER;
    }

    *MofResourceCount = 0;
    *MofResourceInfo = NULL;

    Error = WmipQueryMofList(&List, &ListSize);
    if (Error != ERROR_SUCCESS)
    {
        SetLastError(Error);
        return Error;
    }

    if ((ListSize < sizeof(*List)) || !List->Count ||
        (List->Count > (ListSize - FIELD_OFFSET(WMI_MOF_LIST, Entries)) / sizeof(WMI_MOF_ENTRY)))
    {
        goto Done;
    }

    Images = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, List->Count * sizeof(*Images));
    if (!Images)
    {
        Error = ERROR_NOT_ENOUGH_MEMORY;
        goto Done;
    }

    /* Resolve every image first, entries whose driver cannot be found are left out */
    Size = 0;
    for (Index = 0; Index < List->Count; Index++)
    {
        Path = WmipMofListString(List, ListSize, List->Entries[Index].RegistryPathOffset);
        Name = WmipMofListString(List, ListSize, List->Entries[Index].ResourceNameOffset);
        if (!Path || !Name)
            continue;

        if (List->Entries[Index].Flags & WMI_MOF_ENTRY_IMAGE_PATH)
        {
            Images[Index] = HeapAlloc(GetProcessHeap(), 0, (wcslen(Path) + 1) * sizeof(WCHAR));
            if (Images[Index])
                wcscpy(Images[Index], Path);
        }
        else
        {
            Images[Index] = WmipServiceKeyToImagePath(Path);
        }

        if (!Images[Index])
            continue;

        Count++;
        Size += (wcslen(Images[Index]) + wcslen(Name) + 2) * sizeof(WCHAR);
    }

    if (!Count)
        goto Done;

    Info = HeapAlloc(GetProcessHeap(), 0, Count * sizeof(*Info) + Size);
    if (!Info)
    {
        Error = ERROR_NOT_ENOUGH_MEMORY;
        goto Done;
    }

    Strings = (PWCHAR)(Info + Count);
    Count = 0;
    for (Index = 0; Index < List->Count; Index++)
    {
        if (!Images[Index])
            continue;

        Name = WmipMofListString(List, ListSize, List->Entries[Index].ResourceNameOffset);
        ImageSize = (wcslen(Images[Index]) + 1) * sizeof(WCHAR);
        NameSize = (wcslen(Name) + 1) * sizeof(WCHAR);

        Info[Count].ImagePath = Strings;
        CopyMemory(Strings, Images[Index], ImageSize);
        Strings += ImageSize / sizeof(WCHAR);

        Info[Count].ResourceName = Strings;
        CopyMemory(Strings, Name, NameSize);
        Strings += NameSize / sizeof(WCHAR);

        Info[Count].ResourceSize = 0;
        Info[Count].ResourceBuffer = NULL;
        Count++;
    }

    *MofResourceCount = Count;
    *MofResourceInfo = Info;

Done:
    if (Images)
    {
        for (Index = 0; Index < List->Count; Index++)
        {
            if (Images[Index])
                HeapFree(GetProcessHeap(), 0, Images[Index]);
        }
        HeapFree(GetProcessHeap(), 0, Images);
    }

    HeapFree(GetProcessHeap(), 0, List);
    SetLastError(Error);
    return Error;
}

/*
 * @implemented
 */
ULONG
WINAPI
WmiMofEnumerateResourcesA(
    _In_ MOFHANDLE MofResourceHandle,
    _Out_ ULONG *MofResourceCount,
    _Outptr_result_buffer_(*MofResourceCount) PMOFRESOURCEINFOA *MofResourceInfo)
{
    PMOFRESOURCEINFOW InfoW;
    PMOFRESOURCEINFOA InfoA;
    ULONG Count, Index, Error;
    SIZE_T Size;
    LPSTR Strings;
    int Length;

    if (!MofResourceCount || !MofResourceInfo)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return ERROR_INVALID_PARAMETER;
    }

    *MofResourceCount = 0;
    *MofResourceInfo = NULL;

    Error = WmiMofEnumerateResourcesW(MofResourceHandle, &Count, &InfoW);
    if ((Error != ERROR_SUCCESS) || !Count)
        return Error;

    Size = Count * sizeof(*InfoA);
    for (Index = 0; Index < Count; Index++)
    {
        Size += WideCharToMultiByte(CP_ACP, 0, InfoW[Index].ImagePath, -1, NULL, 0, NULL, NULL);
        Size += WideCharToMultiByte(CP_ACP, 0, InfoW[Index].ResourceName, -1, NULL, 0, NULL, NULL);
    }

    InfoA = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!InfoA)
    {
        WmiFreeBuffer(InfoW);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    Strings = (LPSTR)(InfoA + Count);
    Size -= Count * sizeof(*InfoA);
    for (Index = 0; Index < Count; Index++)
    {
        InfoA[Index].ImagePath = Strings;
        Length = WideCharToMultiByte(CP_ACP, 0, InfoW[Index].ImagePath, -1, Strings, (int)Size, NULL, NULL);
        Strings += Length;
        Size -= Length;

        InfoA[Index].ResourceName = Strings;
        Length = WideCharToMultiByte(CP_ACP, 0, InfoW[Index].ResourceName, -1, Strings, (int)Size, NULL, NULL);
        Strings += Length;
        Size -= Length;

        InfoA[Index].ResourceSize = 0;
        InfoA[Index].ResourceBuffer = NULL;
    }

    WmiFreeBuffer(InfoW);

    *MofResourceCount = Count;
    *MofResourceInfo = InfoA;
    SetLastError(ERROR_SUCCESS);
    return ERROR_SUCCESS;
}

/*
 * @implemented
 */
ULONG
WINAPI
WmiFileHandleToInstanceNameW(
    _In_ WMIHANDLE DataBlockHandle,
    _In_ HANDLE FileHandle,
    _Inout_ ULONG *NumberCharacters,
    _Out_writes_opt_(*NumberCharacters) PWCHAR InstanceNames)
{
    PWMI_FILE_HANDLE_NAME Request;
    ULONG Size = 0x200, ReturnSize, Needed, Error, Try;
    WCHAR Suffix[16];
    SIZE_T PathChars;

    if (!NumberCharacters)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return ERROR_INVALID_PARAMETER;
    }

    for (Try = 0; ; Try++)
    {
        Request = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
        if (!Request)
        {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return ERROR_NOT_ENOUGH_MEMORY;
        }

        Request->FileHandle64 = (ULONG64)(ULONG_PTR)FileHandle;
        Request->KernelHandle64 = (ULONG64)(ULONG_PTR)DataBlockHandle;

        Error = WmipSendKernelRequest(IOCTL_WMI_TRANSLATE_FILE_HANDLE,
                                      Request,
                                      FIELD_OFFSET(WMI_FILE_HANDLE_NAME, InstanceNames),
                                      Request,
                                      Size,
                                      &ReturnSize);
        if ((Error != ERROR_SUCCESS) || (ReturnSize != sizeof(ULONG)) || (Try > 2))
            break;

        Size = Request->SizeNeeded;
        HeapFree(GetProcessHeap(), 0, Request);
    }

    if ((Error == ERROR_SUCCESS) && (ReturnSize <= FIELD_OFFSET(WMI_FILE_HANDLE_NAME, InstanceNames)))
        Error = ERROR_WMI_INSTANCE_NOT_FOUND;

    if (Error == ERROR_SUCCESS)
    {
        /* The name is the device instance path and its index, as a list ending in two terminators */
        Request->InstanceNames[(ReturnSize - FIELD_OFFSET(WMI_FILE_HANDLE_NAME, InstanceNames)) / sizeof(WCHAR) - 1] = UNICODE_NULL;
        _snwprintf(Suffix, RTL_NUMBER_OF(Suffix), L"_%lu", Request->BaseIndex);
        Suffix[RTL_NUMBER_OF(Suffix) - 1] = UNICODE_NULL;

        PathChars = wcslen(Request->InstanceNames);
        Needed = (ULONG)(PathChars + wcslen(Suffix) + 2);

        if (!InstanceNames || (*NumberCharacters < Needed))
        {
            Error = ERROR_INSUFFICIENT_BUFFER;
        }
        else
        {
            CopyMemory(InstanceNames, Request->InstanceNames, PathChars * sizeof(WCHAR));
            wcscpy(InstanceNames + PathChars, Suffix);
            InstanceNames[Needed - 1] = UNICODE_NULL;
        }

        *NumberCharacters = Needed;
    }

    HeapFree(GetProcessHeap(), 0, Request);
    SetLastError(Error);
    return Error;
}

/*
 * @implemented
 */
ULONG
WINAPI
WmiFileHandleToInstanceNameA(
    _In_ WMIHANDLE DataBlockHandle,
    _In_ HANDLE FileHandle,
    _Inout_ ULONG *NumberCharacters,
    _Out_writes_opt_(*NumberCharacters) PCHAR InstanceNames)
{
    ULONG Chars = 0, Error;
    PWCHAR Names;
    int Length;

    if (!NumberCharacters)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return ERROR_INVALID_PARAMETER;
    }

    Error = WmiFileHandleToInstanceNameW(DataBlockHandle, FileHandle, &Chars, NULL);
    if (Error != ERROR_INSUFFICIENT_BUFFER)
        return Error;

    Names = HeapAlloc(GetProcessHeap(), 0, Chars * sizeof(WCHAR));
    if (!Names)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    Error = WmiFileHandleToInstanceNameW(DataBlockHandle, FileHandle, &Chars, Names);
    if (Error == ERROR_SUCCESS)
    {
        Length = WideCharToMultiByte(CP_ACP, 0, Names, Chars, NULL, 0, NULL, NULL);
        if (!InstanceNames || (*NumberCharacters < (ULONG)Length))
            Error = ERROR_INSUFFICIENT_BUFFER;
        else
            WideCharToMultiByte(CP_ACP, 0, Names, Chars, InstanceNames, Length, NULL, NULL);

        *NumberCharacters = Length;
    }

    HeapFree(GetProcessHeap(), 0, Names);
    SetLastError(Error);
    return Error;
}

/* NOTIFICATIONS ************************************************************/

/* One GUID this process listens to, sharing a single kernel handle for all its callbacks */
typedef struct _WMIP_NOTIFY_GUID
{
    LIST_ENTRY Link;
    GUID Guid;
    HANDLE KernelHandle;
    LIST_ENTRY CallbackListHead;
} WMIP_NOTIFY_GUID, *PWMIP_NOTIFY_GUID;

typedef struct _WMIP_NOTIFY_CALLBACK
{
    LIST_ENTRY Link;
    NOTIFICATIONCALLBACK Callback;
    ULONG_PTR Context;
    BOOLEAN Ansi;
} WMIP_NOTIFY_CALLBACK, *PWMIP_NOTIFY_CALLBACK;

/* Copy of a callback taken for delivery outside the lock */
typedef struct _WMIP_DELIVERY
{
    NOTIFICATIONCALLBACK Callback;
    ULONG_PTR Context;
    BOOLEAN Ansi;
} WMIP_DELIVERY, *PWMIP_DELIVERY;

/* First receive buffer of the pump thread, it grows when an event does not fit */
#define WMIP_PUMP_BUFFER_SIZE   0x2000
#define WMIP_PUMP_BUFFER_LIMIT  0x100000

/* How long the pump waits before retrying a failed receive */
#define WMIP_PUMP_RETRY_DELAY   1000

static RTL_CRITICAL_SECTION WmipNotifyLock;
static LIST_ENTRY WmipNotifyListHead;
static HANDLE WmipPumpWakeEvent;
static BOOLEAN WmipPumpRunning;

VOID
WmipInitializeNotifications(
    VOID)
{
    RtlInitializeCriticalSection(&WmipNotifyLock);
    InitializeListHead(&WmipNotifyListHead);
}

static
PWMIP_NOTIFY_GUID
WmipFindNotifyGuidLocked(
    _In_ LPCGUID Guid)
{
    PWMIP_NOTIFY_GUID NotifyGuid;
    PLIST_ENTRY Entry;

    for (Entry = WmipNotifyListHead.Flink; Entry != &WmipNotifyListHead; Entry = Entry->Flink)
    {
        NotifyGuid = CONTAINING_RECORD(Entry, WMIP_NOTIFY_GUID, Link);
        if (IsEqualGUID(&NotifyGuid->Guid, Guid))
            return NotifyGuid;
    }

    return NULL;
}

/* Rewrites the instance names of a WNODE to the ANSI code page, as A callbacks expect them. */
static
VOID
WmipWnodeNamesToAnsi(
    _Inout_ PWNODE_HEADER Wnode)
{
    PWNODE_ALL_DATA AllData = (PWNODE_ALL_DATA)Wnode;
    PWNODE_SINGLE_INSTANCE Single = (PWNODE_SINGLE_INSTANCE)Wnode;
    PULONG NameOffsets;
    ULONG Index;

    if (Wnode->Flags & WNODE_FLAG_ALL_DATA)
    {
        if ((Wnode->BufferSize < FIELD_OFFSET(WNODE_ALL_DATA, OffsetInstanceDataAndLength)) ||
            !AllData->OffsetInstanceNameOffsets ||
            (AllData->OffsetInstanceNameOffsets > Wnode->BufferSize) ||
            (AllData->InstanceCount > (Wnode->BufferSize - AllData->OffsetInstanceNameOffsets) / sizeof(ULONG)))
        {
            return;
        }

        NameOffsets = (PULONG)((PUCHAR)Wnode + AllData->OffsetInstanceNameOffsets);
        for (Index = 0; Index < AllData->InstanceCount; Index++)
        {
            if (NameOffsets[Index] && (NameOffsets[Index] <= Wnode->BufferSize - sizeof(USHORT)))
                WmipCountedNameToAnsi((PUSHORT)((PUCHAR)Wnode + NameOffsets[Index]));
        }
    }
    else if (Wnode->Flags & (WNODE_FLAG_SINGLE_INSTANCE | WNODE_FLAG_SINGLE_ITEM | WNODE_FLAG_METHOD_ITEM))
    {
        if ((Wnode->BufferSize < FIELD_OFFSET(WNODE_SINGLE_INSTANCE, VariableData)) ||
            !Single->OffsetInstanceName ||
            (Single->OffsetInstanceName > Wnode->BufferSize - sizeof(USHORT)))
        {
            return;
        }

        WmipCountedNameToAnsi((PUSHORT)((PUCHAR)Wnode + Single->OffsetInstanceName));
    }
    else
    {
        return;
    }

    Wnode->Flags |= WNODE_FLAG_ANSI_INSTANCENAMES;
}

/* Calls every callback registered for the GUID of one received event. */
static
VOID
WmipDispatchEvent(
    _Inout_ PWNODE_HEADER Wnode)
{
    PWMIP_DELIVERY Deliveries = NULL;
    PWMIP_NOTIFY_CALLBACK Callback;
    PWMIP_NOTIFY_GUID NotifyGuid;
    PWNODE_HEADER Copy;
    PLIST_ENTRY Entry;
    ULONG Count = 0, Index;

    RtlEnterCriticalSection(&WmipNotifyLock);

    NotifyGuid = WmipFindNotifyGuidLocked(&Wnode->Guid);
    if (NotifyGuid)
    {
        for (Entry = NotifyGuid->CallbackListHead.Flink; Entry != &NotifyGuid->CallbackListHead; Entry = Entry->Flink)
            Count++;

        Deliveries = HeapAlloc(GetProcessHeap(), 0, Count * sizeof(*Deliveries));
        if (Deliveries)
        {
            Index = 0;
            for (Entry = NotifyGuid->CallbackListHead.Flink;
                 Entry != &NotifyGuid->CallbackListHead;
                 Entry = Entry->Flink)
            {
                Callback = CONTAINING_RECORD(Entry, WMIP_NOTIFY_CALLBACK, Link);
                Deliveries[Index].Callback = Callback->Callback;
                Deliveries[Index].Context = Callback->Context;
                Deliveries[Index].Ansi = Callback->Ansi;
                Index++;
            }
        }
    }

    RtlLeaveCriticalSection(&WmipNotifyLock);

    if (!Deliveries)
        return;

    /* Callbacks run unlocked so they may register or unregister themselves */
    for (Index = 0; Index < Count; Index++)
    {
        Copy = Wnode;
        if (Deliveries[Index].Ansi)
        {
            Copy = HeapAlloc(GetProcessHeap(), 0, Wnode->BufferSize);
            if (!Copy)
                continue;

            CopyMemory(Copy, Wnode, Wnode->BufferSize);
            WmipWnodeNamesToAnsi(Copy);
        }

        Deliveries[Index].Callback(Copy, Deliveries[Index].Context);

        if (Copy != Wnode)
            HeapFree(GetProcessHeap(), 0, Copy);
    }

    HeapFree(GetProcessHeap(), 0, Deliveries);
}

/* Walks a received WNODE chain, each event leaves the chain before its callbacks see it. */
static
VOID
WmipDispatchEvents(
    _Inout_updates_bytes_(Size) PUCHAR Buffer,
    _In_ ULONG Size)
{
    PWNODE_HEADER Wnode;
    ULONG Offset = 0, Next;

    while (Offset <= Size - sizeof(WNODE_HEADER))
    {
        Wnode = (PWNODE_HEADER)(Buffer + Offset);
        if ((Wnode->BufferSize < sizeof(WNODE_HEADER)) || (Wnode->BufferSize > Size - Offset))
            break;

        Next = Wnode->Linkage;
        Wnode->Linkage = 0;
        WmipDispatchEvent(Wnode);

        if (!Next || (Next > Size - Offset) || (Next & 7))
            break;
        Offset += Next;
    }
}

/* Snapshot of the kernel handles of every GUID listened to, NULL when there are none left. */
static
PWMI_NOTIFICATION_HANDLES
WmipBuildReceiveRequestLocked(
    _Out_ PULONG RequestSize)
{
    PWMI_NOTIFICATION_HANDLES Request;
    PWMIP_NOTIFY_GUID NotifyGuid;
    PLIST_ENTRY Entry;
    ULONG Count = 0;

    for (Entry = WmipNotifyListHead.Flink; Entry != &WmipNotifyListHead; Entry = Entry->Flink)
        Count++;

    *RequestSize = 0;
    if (!Count)
        return NULL;

    *RequestSize = FIELD_OFFSET(WMI_NOTIFICATION_HANDLES, Handles) + Count * sizeof(ULONG64);
    Request = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, *RequestSize);
    if (!Request)
        return NULL;

    Request->HandleCount = 0;
    for (Entry = WmipNotifyListHead.Flink; Entry != &WmipNotifyListHead; Entry = Entry->Flink)
    {
        NotifyGuid = CONTAINING_RECORD(Entry, WMIP_NOTIFY_GUID, Link);
        Request->Handles[Request->HandleCount++] = (ULONG64)(ULONG_PTR)NotifyGuid->KernelHandle;
    }

    return Request;
}

/**
 * @brief Waits for events on every GUID the process listens to and hands them to the callbacks.
 *
 * Registration changes wake it to restart the wait with the new handle set, it ends with the last GUID.
 */
static
DWORD
WINAPI
WmipNotificationPump(
    _In_ LPVOID Parameter)
{
    PWMI_NOTIFICATION_HANDLES Request;
    ULONG BufferSize = WMIP_PUMP_BUFFER_SIZE;
    ULONG RequestSize, Needed, Error;
    OVERLAPPED Overlapped;
    HANDLE Waits[2];
    DWORD Returned, Wait;
    PUCHAR Buffer;
    HANDLE Device;

    UNREFERENCED_PARAMETER(Parameter);

    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Buffer = HeapAlloc(GetProcessHeap(), 0, BufferSize);

    for (;;)
    {
        RtlEnterCriticalSection(&WmipNotifyLock);
        Request = WmipBuildReceiveRequestLocked(&RequestSize);
        if (!RequestSize || !Overlapped.hEvent || !Buffer || (WmipGetKernelDevice(&Device) != ERROR_SUCCESS))
        {
            /* Registrations that come later start a new pump */
            WmipPumpRunning = FALSE;
            RtlLeaveCriticalSection(&WmipNotifyLock);
            if (Request)
                HeapFree(GetProcessHeap(), 0, Request);
            break;
        }
        RtlLeaveCriticalSection(&WmipNotifyLock);

        if (!Request)
        {
            WaitForSingleObject(WmipPumpWakeEvent, WMIP_PUMP_RETRY_DELAY);
            continue;
        }

        Error = ERROR_SUCCESS;
        Returned = 0;
        ResetEvent(Overlapped.hEvent);
        if (!DeviceIoControl(Device,
                             IOCTL_WMI_RECEIVE_NOTIFICATIONS,
                             Request,
                             RequestSize,
                             Buffer,
                             BufferSize,
                             &Returned,
                             &Overlapped))
        {
            Error = GetLastError();
            if (Error == ERROR_IO_PENDING)
            {
                Waits[0] = Overlapped.hEvent;
                Waits[1] = WmipPumpWakeEvent;
                Wait = WaitForMultipleObjects(RTL_NUMBER_OF(Waits), Waits, FALSE, INFINITE);
                if (Wait != WAIT_OBJECT_0)
                    CancelIo(Device);

                Error = ERROR_SUCCESS;
                if (!GetOverlappedResult(Device, &Overlapped, &Returned, TRUE))
                    Error = GetLastError();
            }
        }

        HeapFree(GetProcessHeap(), 0, Request);

        if (Error == ERROR_SUCCESS)
        {
            if ((Returned >= sizeof(WNODE_TOO_SMALL)) &&
                (((PWNODE_HEADER)Buffer)->Flags & WNODE_FLAG_TOO_SMALL))
            {
                /* The events stay queued in the kernel for a larger buffer */
                Needed = ((PWNODE_TOO_SMALL)Buffer)->SizeNeeded;
                if ((Needed > BufferSize) && (Needed <= WMIP_PUMP_BUFFER_LIMIT))
                {
                    HeapFree(GetProcessHeap(), 0, Buffer);
                    BufferSize = (Needed + 0xFFF) & ~0xFFF;
                    Buffer = HeapAlloc(GetProcessHeap(), 0, BufferSize);
                }
                else
                {
                    WaitForSingleObject(WmipPumpWakeEvent, WMIP_PUMP_RETRY_DELAY);
                }
                continue;
            }

            if (Returned >= sizeof(WNODE_HEADER))
                WmipDispatchEvents(Buffer, Returned);
        }
        else if (Error != ERROR_OPERATION_ABORTED)
        {
            /* A handle closed under the request, the next pass picks up the current set */
            WaitForSingleObject(WmipPumpWakeEvent, (Error == ERROR_INVALID_HANDLE) ? 0 : WMIP_PUMP_RETRY_DELAY);
        }
    }

    if (Buffer)
        HeapFree(GetProcessHeap(), 0, Buffer);
    if (Overlapped.hEvent)
        CloseHandle(Overlapped.hEvent);

    return 0;
}

/* Makes sure a pump thread runs, or tells the running one the handle set changed. */
static
ULONG
WmipKickPumpLocked(
    VOID)
{
    HANDLE Thread;

    if (!WmipPumpWakeEvent)
    {
        WmipPumpWakeEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!WmipPumpWakeEvent)
            return GetLastError();
    }

    if (WmipPumpRunning)
    {
        SetEvent(WmipPumpWakeEvent);
        return ERROR_SUCCESS;
    }

    Thread = CreateThread(NULL, 0, WmipNotificationPump, NULL, 0, NULL);
    if (!Thread)
        return GetLastError();

    CloseHandle(Thread);
    WmipPumpRunning = TRUE;
    return ERROR_SUCCESS;
}

static
ULONG
WmipNotificationRegistration(
    _In_ LPGUID Guid,
    _In_ BOOLEAN Enable,
    _In_ PVOID DeliveryInfo,
    _In_ ULONG_PTR DeliveryContext,
    _In_ ULONG Flags,
    _In_ BOOLEAN Ansi)
{
    PWMIP_NOTIFY_CALLBACK Callback = NULL;
    PWMIP_NOTIFY_GUID NotifyGuid;
    PLIST_ENTRY Entry, Next;
    BOOLEAN Removed = FALSE;
    WMIHANDLE Handle;
    ULONG Error;

    if (!Guid)
        return ERROR_INVALID_PARAMETER;

    /* Enabling trace providers belongs to event tracing */
    if (Flags & NOTIFICATION_TRACE_FLAG)
        return ERROR_NOT_SUPPORTED;

    /* Only tells whether the caller may listen to the GUID */
    if (Flags & NOTIFICATION_CHECK_ACCESS)
    {
        Error = WmiOpenBlock(Guid, WMIGUID_NOTIFICATION, &Handle);
        if (Error == ERROR_SUCCESS)
            WmiCloseBlock(Handle);
        return Error;
    }

    RtlEnterCriticalSection(&WmipNotifyLock);

    NotifyGuid = WmipFindNotifyGuidLocked(Guid);

    if (!Enable)
    {
        Error = ERROR_WMI_ALREADY_DISABLED;
        if (NotifyGuid)
        {
            for (Entry = NotifyGuid->CallbackListHead.Flink; Entry != &NotifyGuid->CallbackListHead; Entry = Next)
            {
                Next = Entry->Flink;
                Callback = CONTAINING_RECORD(Entry, WMIP_NOTIFY_CALLBACK, Link);
                if (DeliveryInfo && ((PVOID)Callback->Callback != DeliveryInfo))
                    continue;

                RemoveEntryList(Entry);
                HeapFree(GetProcessHeap(), 0, Callback);
                Removed = TRUE;
            }

            if (Removed)
                Error = ERROR_SUCCESS;

            /* Closing the handle fails the waiting receive, the pump then drops the GUID */
            if (IsListEmpty(&NotifyGuid->CallbackListHead))
            {
                RemoveEntryList(&NotifyGuid->Link);
                CloseHandle(NotifyGuid->KernelHandle);
                HeapFree(GetProcessHeap(), 0, NotifyGuid);
                if (WmipPumpRunning)
                    SetEvent(WmipPumpWakeEvent);
            }
        }

        RtlLeaveCriticalSection(&WmipNotifyLock);
        return Error;
    }

    if (!DeliveryInfo)
    {
        RtlLeaveCriticalSection(&WmipNotifyLock);
        return ERROR_INVALID_PARAMETER;
    }

    if (!NotifyGuid)
    {
        NotifyGuid = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*NotifyGuid));
        if (!NotifyGuid)
        {
            RtlLeaveCriticalSection(&WmipNotifyLock);
            return ERROR_NOT_ENOUGH_MEMORY;
        }

        Error = WmiOpenBlock(Guid, WMIGUID_NOTIFICATION, &NotifyGuid->KernelHandle);
        if (Error != ERROR_SUCCESS)
        {
            HeapFree(GetProcessHeap(), 0, NotifyGuid);
            RtlLeaveCriticalSection(&WmipNotifyLock);
            return Error;
        }

        NotifyGuid->Guid = *Guid;
        InitializeListHead(&NotifyGuid->CallbackListHead);
        InsertTailList(&WmipNotifyListHead, &NotifyGuid->Link);
    }

    /* Registering a callback again only updates its context */
    for (Entry = NotifyGuid->CallbackListHead.Flink; Entry != &NotifyGuid->CallbackListHead; Entry = Entry->Flink)
    {
        Callback = CONTAINING_RECORD(Entry, WMIP_NOTIFY_CALLBACK, Link);
        if ((PVOID)Callback->Callback == DeliveryInfo)
            break;
        Callback = NULL;
    }

    if (!Callback)
    {
        Callback = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Callback));
        if (!Callback)
        {
            Error = ERROR_NOT_ENOUGH_MEMORY;
            goto Undo;
        }

        Callback->Callback = (NOTIFICATIONCALLBACK)DeliveryInfo;
        InsertTailList(&NotifyGuid->CallbackListHead, &Callback->Link);
    }

    Callback->Context = DeliveryContext;
    Callback->Ansi = Ansi;

    Error = WmipKickPumpLocked();
    if (Error == ERROR_SUCCESS)
    {
        RtlLeaveCriticalSection(&WmipNotifyLock);
        return ERROR_SUCCESS;
    }

    RemoveEntryList(&Callback->Link);
    HeapFree(GetProcessHeap(), 0, Callback);

Undo:
    if (IsListEmpty(&NotifyGuid->CallbackListHead))
    {
        RemoveEntryList(&NotifyGuid->Link);
        CloseHandle(NotifyGuid->KernelHandle);
        HeapFree(GetProcessHeap(), 0, NotifyGuid);
    }

    RtlLeaveCriticalSection(&WmipNotifyLock);
    return Error;
}

/*
 * @implemented
 */
ULONG
WINAPI
WmiNotificationRegistrationW(
    _In_ LPGUID Guid,
    _In_ BOOLEAN Enable,
    _In_ PVOID DeliveryInfo,
    _In_ ULONG_PTR DeliveryContext,
    _In_ ULONG Flags)
{
    ULONG Error;

    Error = WmipNotificationRegistration(Guid, Enable, DeliveryInfo, DeliveryContext, Flags, FALSE);
    SetLastError(Error);
    return Error;
}

/*
 * @implemented
 */
ULONG
WINAPI
WmiNotificationRegistrationA(
    _In_ LPGUID Guid,
    _In_ BOOLEAN Enable,
    _In_ PVOID DeliveryInfo,
    _In_ ULONG_PTR DeliveryContext,
    _In_ ULONG Flags)
{
    ULONG Error;

    Error = WmipNotificationRegistration(Guid, Enable, DeliveryInfo, DeliveryContext, Flags, TRUE);
    SetLastError(Error);
    return Error;
}
