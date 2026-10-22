/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS Kernel Streaming
 * FILE:            dll/directx/ksuser/ksuser.c
 * PURPOSE:         KS USER functions
 * PROGRAMMER:      Magnus Olsen and Dmitry Chapyshev and Johannes Anderwald
 */

#include "ksuser.h"
#define NDEBUG
#include <debug.h>

NTSTATUS
NTAPI
KsiCreateObjectType( HANDLE hHandle,
                     LPWSTR ObjectType,
                     PVOID Buffer,
                     ULONG BufferSize,
                     ACCESS_MASK DesiredAccess,
                     PHANDLE phHandle)
{
    NTSTATUS Status;
    SIZE_T Length;
    SIZE_T TotalSize;
    LPWSTR pStr;
    UNICODE_STRING ObjectName;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;

    /* get length of object type */
    Length = wcslen(ObjectType);

    /* get length for request */
    TotalSize = (Length * sizeof(WCHAR)) + BufferSize;

    /* append space for '\\'*/
    TotalSize += sizeof(WCHAR);

    /* allocate buffer */
    pStr = HeapAlloc(GetProcessHeap(), 0, TotalSize);
    if (!pStr)
    {
        /* out of memory */
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    /* copy object type */
    wcscpy(pStr, ObjectType);

    /* append slash */
    pStr[Length] = L'\\';

    /* append parameters */
    memcpy(&pStr[Length+1], Buffer, BufferSize);

    /* initialize object name */
    ObjectName.Buffer = pStr;
    ObjectName.Length = ObjectName.MaximumLength = TotalSize;

    /* initialize object attributes */
    InitializeObjectAttributes(&ObjectAttributes, &ObjectName, OBJ_CASE_INSENSITIVE, hHandle, NULL);

    /* create the object */
    Status = NtCreateFile(phHandle, DesiredAccess, &ObjectAttributes, &IoStatusBlock, NULL, FILE_ATTRIBUTE_NORMAL, 0, 1, 0, NULL, 0);

    /* free buffer */
    HeapFree(GetProcessHeap(), 0, pStr);

    /* check for success */
    if (!NT_SUCCESS(Status))
    {
        /* failed zero handle */
        *phHandle = INVALID_HANDLE_VALUE;

        /* convert error code */
        Status = RtlNtStatusToDosError(Status);
    }

    /* done */
    return Status;
}

/**
 * @brief Opens a KS object below a parent handle, returning an HRESULT.
 * The name is "<ObjectType>\<Buffer>" and must fit a UNICODE_STRING.
 */
static
HRESULT
KsiCreateObjectHandle(
    _In_ HANDLE ParentHandle,
    _In_ PCWSTR ObjectType,
    _In_reads_bytes_(BufferSize) PVOID Buffer,
    _In_ ULONG BufferSize,
    _In_ ACCESS_MASK DesiredAccess,
    _Out_ PHANDLE ObjectHandle)
{
    SIZE_T TypeChars;
    SIZE_T NameBytes;
    PWSTR Name;
    UNICODE_STRING ObjectName;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS Status;

    TypeChars = wcslen(ObjectType);
    NameBytes = (TypeChars + 1) * sizeof(WCHAR) + BufferSize;
    NameBytes = (NameBytes + 1) & ~(SIZE_T)1;
    if (NameBytes > MAXUSHORT)
        return E_FAIL;

    if (ParentHandle == INVALID_HANDLE_VALUE || !ObjectHandle)
        return E_INVALIDARG;

    *ObjectHandle = INVALID_HANDLE_VALUE;

    Name = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, NameBytes);
    if (!Name)
        return E_OUTOFMEMORY;

    RtlCopyMemory(Name, ObjectType, TypeChars * sizeof(WCHAR));
    Name[TypeChars] = L'\\';
    RtlCopyMemory(&Name[TypeChars + 1], Buffer, BufferSize);

    ObjectName.Buffer = Name;
    ObjectName.Length = (USHORT)NameBytes;
    ObjectName.MaximumLength = (USHORT)NameBytes;
    InitializeObjectAttributes(&ObjectAttributes,
                               &ObjectName,
                               OBJ_CASE_INSENSITIVE,
                               ParentHandle,
                               NULL);

    Status = NtCreateFile(ObjectHandle,
                          DesiredAccess | SYNCHRONIZE | FILE_READ_ATTRIBUTES,
                          &ObjectAttributes,
                          &IoStatusBlock,
                          NULL,
                          FILE_ATTRIBUTE_NORMAL,
                          0,
                          FILE_OPEN,
                          0,
                          NULL,
                          0);

    HeapFree(GetProcessHeap(), 0, Name);

    if (!NT_SUCCESS(Status))
    {
        *ObjectHandle = INVALID_HANDLE_VALUE;
        return HRESULT_FROM_NT(Status);
    }

    return S_OK;
}

/**
 * @brief Returns the size of a pin connect request, including the data
 * format and the attribute list that follows it when present.
 */
static
ULONG
KsiGetPinConnectSize(
    _In_ PKSPIN_CONNECT Connect)
{
    PKSDATAFORMAT DataFormat = (PKSDATAFORMAT)(Connect + 1);
    PKSMULTIPLE_ITEM Attributes;
    ULONG Size;

    Size = DataFormat->FormatSize;
    if (DataFormat->Flags & KSDATAFORMAT_ATTRIBUTES)
    {
        Size = ROUND_UP(Size, sizeof(LONGLONG));
        Attributes = (PKSMULTIPLE_ITEM)((PUCHAR)DataFormat + Size);
        Size += Attributes->Size;
    }

    return sizeof(*Connect) + Size;
}

/*++
* @name KsCreateAllocator
* @implemented
* The function KsCreateAllocator creates a handle to an allocator for the given sink connection handle
*
* @param HANDLE ConnectionHandle
* Handle to the sink connection on which to create the allocator
*
* @param PKSALLOCATOR_FRAMING AllocatorFraming
* the input param we using to alloc our framing
*
* @param PHANDLE AllocatorHandle
* Our new handle that we have alloc
*
* @return
* Return NTSTATUS error code or sussess code.
*
* @remarks.
* none
*
*--*/
KSDDKAPI
DWORD
NTAPI
KsCreateAllocator(HANDLE ConnectionHandle,
                  PKSALLOCATOR_FRAMING AllocatorFraming,
                  PHANDLE AllocatorHandle)

{
    return KsiCreateObjectType( ConnectionHandle,
                                KSSTRING_Allocator,
                                (PVOID) AllocatorFraming,
                                sizeof(KSALLOCATOR_FRAMING),
                                GENERIC_READ,
                                AllocatorHandle);
}

/*++
* @name KsCreateClock
* @implemented
*
* The function KsCreateClock  creates handle to clock instance
*
* @param HANDLE ConnectionHandle
* Handle to use to create the clock
*
* @param PKSCLOCK_CREATE ClockCreate
* parameter to use to create the clock
*
* @param PHANDLE  ClockHandle
* The new handle
*
* @return
* Return NTSTATUS error code or sussess code.
*
* @remarks.
* none
*
*--*/
KSDDKAPI
DWORD
NTAPI
KsCreateClock(HANDLE ConnectionHandle,
              PKSCLOCK_CREATE ClockCreate,
              PHANDLE  ClockHandle)
{
    return KsiCreateObjectType( ConnectionHandle,
                                KSSTRING_Clock,
                                (PVOID) ClockCreate,
                                sizeof(KSCLOCK_CREATE),
                                GENERIC_READ,
                                ClockHandle);
}

/*++
* @name KsCreatePin
* @implemented
*
* The function KsCreatePin passes a connection request to device and create pin instance
*
* @param HANDLE FilterHandle
* handle of the filter initiating the create request
*
* @param PKSPIN_CONNECT Connect
* Pointer to a KSPIN_CONNECT structure that contains parameters for the requested connection.
* This should be followed in memory by a KSDATAFORMAT data structure, describing the data format
* requested for the connection.

* @param ACCESS_MASK  DesiredAccess
* Desired access
*
* @param PHANDLE ConnectionHandle
* connection handle passed
*
* @return
* Return NTSTATUS error code or sussess code.
*
* @remarks.
* The flag in PKSDATAFORMAT is not really document,
* to find it u need api monitor allot api and figout
* how it works, only flag I have found is the
* KSDATAFORMAT_ATTRIBUTES flag, it doing a Align
* of LONLONG size, it also round up it.
*
*--*/

KSDDKAPI
DWORD
NTAPI
KsCreatePin(HANDLE FilterHandle,
            PKSPIN_CONNECT Connect,
            ACCESS_MASK DesiredAccess,
            PHANDLE  ConnectionHandle)
{
    ULONG BufferSize = KsiGetPinConnectSize(Connect);

    return KsiCreateObjectType(FilterHandle,
                               KSSTRING_Pin,
                               Connect,
                               BufferSize,
                               DesiredAccess,
                               ConnectionHandle);

}

/*++
* @name KsCreateTopologyNode
* @implemented
*
* The function KsCreateTopologyNode  creates a handle to a topology node instance
*
* @param HANDLE ParentHandle
* Handle to parent when want to use when we created the node on
*
*
* @param PKSNODE_CREATE  NodeCreate
* topology node parameters to use when it is create
*
* @param ACCESS_MASK  DesiredAccess
* Desired access
*
* @param PHANDLE  NodeHandle
* Location for the topology node handle
*
* @return
* Return NTSTATUS error code or sussess code.
*
* @remarks.
* none
*
*--*/
KSDDKAPI
DWORD
NTAPI
KsCreateTopologyNode(HANDLE ParentHandle,
                     PKSNODE_CREATE NodeCreate,
                     IN ACCESS_MASK DesiredAccess,
                     OUT PHANDLE NodeHandle)
{
    return KsiCreateObjectType( ParentHandle,
                                KSSTRING_TopologyNode,
                                (PVOID) NodeCreate,
                                sizeof(KSNODE_CREATE),
                                DesiredAccess,
                                NodeHandle);
}

/**
 * @brief Creates an allocator on a connection, returning an HRESULT.
 */
KSDDKAPI
HRESULT
WINAPI
KsCreateAllocator2(
    _In_ HANDLE ConnectionHandle,
    _In_ PKSALLOCATOR_FRAMING AllocatorFraming,
    _Out_ PHANDLE AllocatorHandle)
{
    return KsiCreateObjectHandle(ConnectionHandle,
                                 KSSTRING_Allocator,
                                 AllocatorFraming,
                                 sizeof(*AllocatorFraming),
                                 GENERIC_READ,
                                 AllocatorHandle);
}

/**
 * @brief Creates a clock on a connection, returning an HRESULT.
 */
KSDDKAPI
HRESULT
WINAPI
KsCreateClock2(
    _In_ HANDLE ConnectionHandle,
    _In_ PKSCLOCK_CREATE ClockCreate,
    _Out_ PHANDLE ClockHandle)
{
    return KsiCreateObjectHandle(ConnectionHandle,
                                 KSSTRING_Clock,
                                 ClockCreate,
                                 sizeof(*ClockCreate),
                                 GENERIC_READ,
                                 ClockHandle);
}

/**
 * @brief Creates a pin instance on a filter, returning an HRESULT.
 */
KSDDKAPI
HRESULT
WINAPI
KsCreatePin2(
    _In_ HANDLE FilterHandle,
    _In_ PKSPIN_CONNECT Connect,
    _In_ ACCESS_MASK DesiredAccess,
    _Out_ PHANDLE ConnectionHandle)
{
    return KsiCreateObjectHandle(FilterHandle,
                                 KSSTRING_Pin,
                                 Connect,
                                 KsiGetPinConnectSize(Connect),
                                 DesiredAccess,
                                 ConnectionHandle);
}

/**
 * @brief Creates a topology node instance, returning an HRESULT.
 */
KSDDKAPI
HRESULT
WINAPI
KsCreateTopologyNode2(
    _In_ HANDLE ParentHandle,
    _In_ PKSNODE_CREATE NodeCreate,
    _In_ ACCESS_MASK DesiredAccess,
    _Out_ PHANDLE NodeHandle)
{
    return KsiCreateObjectHandle(ParentHandle,
                                 KSSTRING_TopologyNode,
                                 NodeCreate,
                                 sizeof(*NodeCreate),
                                 DesiredAccess,
                                 NodeHandle);
}


BOOL
APIENTRY
DllMain(HANDLE hModule, DWORD ulreason, LPVOID lpReserved)
{
    switch (ulreason)
    {
        case DLL_PROCESS_ATTACH:
        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
        case DLL_PROCESS_DETACH:
            break;
    }

    return TRUE;
}
