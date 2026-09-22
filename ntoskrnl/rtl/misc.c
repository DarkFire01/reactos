/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS kernel
 * FILE:            ntoskrnl/rtl/misc.c
 * PURPOSE:         Various functions
 *
 * PROGRAMMERS:
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#include <sha1.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

extern ULONG NtGlobalFlag;
extern ULONG NtMajorVersion;
extern ULONG NtMinorVersion;
extern ULONG NtOSCSDVersion;
#define PRODUCT_TAG 'iPtR'

/* FUNCTIONS *****************************************************************/

/*
* @implemented
*/
ULONG
NTAPI
RtlGetNtGlobalFlags(VOID)
{
    return NtGlobalFlag;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
RtlGetVersion(IN OUT PRTL_OSVERSIONINFOW lpVersionInformation)
{
    PAGED_CODE();

    /* Return the basics */
    lpVersionInformation->dwMajorVersion = NtMajorVersion;
    lpVersionInformation->dwMinorVersion = NtMinorVersion;
    lpVersionInformation->dwBuildNumber = NtBuildNumber & 0x3FFF;
    lpVersionInformation->dwPlatformId = VER_PLATFORM_WIN32_NT;

    /* Check if this is the extended version */
    if (lpVersionInformation->dwOSVersionInfoSize == sizeof(RTL_OSVERSIONINFOEXW))
    {
        PRTL_OSVERSIONINFOEXW InfoEx = (PRTL_OSVERSIONINFOEXW)lpVersionInformation;
        InfoEx->wServicePackMajor = (USHORT)(CmNtCSDVersion >> 8) & 0xFF;
        InfoEx->wServicePackMinor = (USHORT)(CmNtCSDVersion & 0xFF);
        InfoEx->wSuiteMask = (USHORT)(SharedUserData->SuiteMask & 0xFFFF);
        InfoEx->wProductType = SharedUserData->NtProductType;
        InfoEx->wReserved = 0;
    }

    /* Always succeed */
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Reads the NT product type from the ProductOptions key.
 *
 * @param[out]  ProductType
 *      Receives the product type when the call succeeds.
 *
 * @return
 *      TRUE if a known product type was found, FALSE otherwise.
 */
static
BOOLEAN
RtlpQueryProductTypeValue(
    _Out_ PNT_PRODUCT_TYPE ProductType)
{
    HANDLE Key;
    BOOLEAN Success;
    ULONG ReturnedLength;
    OBJECT_ATTRIBUTES ObjectAttributes;
    NTSTATUS Status;
    PKEY_VALUE_PARTIAL_INFORMATION BufferKey;
    ULONG BufferKeyLength = sizeof(PKEY_VALUE_PARTIAL_INFORMATION) + (256 * sizeof(WCHAR));
    UNICODE_STRING NtProductType;
    static UNICODE_STRING WorkstationProduct = RTL_CONSTANT_STRING(L"WinNT");
    static UNICODE_STRING LanManProduct = RTL_CONSTANT_STRING(L"LanmanNT");
    static UNICODE_STRING ServerProduct = RTL_CONSTANT_STRING(L"ServerNT");
    static UNICODE_STRING KeyProduct = RTL_CONSTANT_STRING(L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\ProductOptions");
    static UNICODE_STRING ValueNtProduct = RTL_CONSTANT_STRING(L"ProductType");

    PAGED_CODE();

    /* Before doing anything else we must allocate some buffer space in the pool */
    BufferKey = ExAllocatePoolWithTag(PagedPool, BufferKeyLength, PRODUCT_TAG);
    if (!BufferKey)
    {
        /* We failed to allocate pool memory, bail out */
        DPRINT1("RtlGetNtProductType(): Memory pool allocation has failed!\n");
        Success = FALSE;
        goto Exit;
    }

    /* Initialize the object attributes to open our key */
    InitializeObjectAttributes(&ObjectAttributes,
                               &KeyProduct,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);

    /* Open the key */
    Status = ZwOpenKey(&Key, KEY_QUERY_VALUE, &ObjectAttributes);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("RtlGetNtProductType(): The ZwOpenKey() function has failed! (Status: 0x%lx)\n", Status);
        Success = FALSE;
        goto Exit;
    }

    /* Now it's time to query the value from the key to check what kind of product the OS is */
    Status = ZwQueryValueKey(Key,
                             &ValueNtProduct,
                             KeyValuePartialInformation,
                             BufferKey,
                             BufferKeyLength,
                             &ReturnedLength);

    /* Free the key from the memory */
    ZwClose(Key);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("RtlGetNtProductType(): The ZwQueryValueKey() function has failed! (Status: 0x%lx)\n", Status);
        Success = FALSE;
        goto Exit;
    }

    /*
     * Do a sanity check before we get the product type of the operating system
     * so that we're sure the type of the value we've got is actually correct.
     */
    if (BufferKey->Type != REG_SZ)
    {
        /* We've got something else, so bail out */
        DPRINT1("RtlGetNtProductType(): An invalid value type has been found!\n");
        Success = FALSE;
        goto Exit;
    }

    /* Initialise the Unicode string with the data from the registry value */
    NtProductType.Length = NtProductType.MaximumLength = BufferKey->DataLength;
    NtProductType.Buffer = (PWCHAR)BufferKey->Data;

    if (NtProductType.Length > sizeof(WCHAR) && NtProductType.Buffer[NtProductType.Length /
        sizeof(WCHAR) - 1] == UNICODE_NULL)
    {
        NtProductType.Length -= sizeof(WCHAR);
    }

    /* Now it's time to get the product based on the value data */
    if (RtlCompareUnicodeString(&NtProductType, &WorkstationProduct, TRUE) == 0)
    {
        /* The following product is an OS Workstation */
        *ProductType = NtProductWinNt;
        Success = TRUE;
    }
    else if (RtlCompareUnicodeString(&NtProductType, &LanManProduct, TRUE) == 0)
    {
        /* The following product is an OS Advanced Server */
        *ProductType = NtProductLanManNt;
        Success = TRUE;
    }
    else if (RtlCompareUnicodeString(&NtProductType, &ServerProduct, TRUE) == 0)
    {
        /* The following product is an OS Server */
        *ProductType = NtProductServer;
        Success = TRUE;
    }
    else
    {
        /* None of the product types match so bail out */
        DPRINT1("RtlGetNtProductType(): Couldn't find a valid product type! Defaulting to WinNT...\n");
        Success = FALSE;
        goto Exit;
    }

Exit:
    if (BufferKey)
        ExFreePoolWithTag(BufferKey, PRODUCT_TAG);

    return Success;
}

/**
 * @brief
 * Retrieves the NT type product of the operating system. This is the kernel-mode variant
 * of this function.
 *
 * @param[out]  ProductType
 *      The NT type product enumeration value returned by the call.
 *
 * @return
 *      The function returns TRUE when the call successfully returned the type product of the system.
 *      It'll return FALSE on failure otherwise. In the latter case the function will return WinNT
 *      as the default product type.
 *
 * @remarks
 *      Once the memory manager has published the product type in the shared user data it is
 *      returned from there at any IRQL. Before that the registry is read, which is only possible
 *      at APC_LEVEL or below.
 */
BOOLEAN
NTAPI
RtlGetNtProductType(
    _Out_ PNT_PRODUCT_TYPE ProductType)
{
    if (SharedUserData->ProductTypeIsValid)
    {
        *ProductType = SharedUserData->NtProductType;
        return TRUE;
    }

    if ((KeGetCurrentIrql() <= APC_LEVEL) && RtlpQueryProductTypeValue(ProductType))
        return TRUE;

    *ProductType = NtProductWinNt;
    return FALSE;
}

/**
 * @brief
 * Returns the ID of the session attached to the physical console.
 */
ULONG
NTAPI
RtlGetActiveConsoleId(VOID)
{
    return SharedUserData->ActiveConsoleId;
}

/**
 * @brief
 * Tells whether this SKU allows more than one interactive session.
 *
 * @remarks
 * The NT 5.2 layout of the shared user data calls the global flags TraceLogging.
 */
BOOLEAN
NTAPI
RtlIsMultiSessionSku(VOID)
{
    return (SharedUserData->TraceLogging & SHARED_GLOBAL_FLAGS_MULTI_SESSION_SKU) != 0;
}

/**
 * @brief
 * Creates a name based GUID, RFC 4122 version 5, from a namespace GUID and a name.
 *
 * @param[in] NamespaceGuid
 * The namespace the name belongs to.
 *
 * @param[in] Buffer
 * The name, hashed as raw bytes.
 *
 * @param[in] BufferSize
 * The size of @p Buffer, in bytes.
 *
 * @param[out] Guid
 * Receives the generated GUID.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INVALID_PARAMETER_1, _3 or _4 for a missing argument.
 */
NTSTATUS
NTAPI
RtlGenerateClass5Guid(
    _In_ REFGUID NamespaceGuid,
    _In_reads_bytes_(BufferSize) PVOID Buffer,
    _In_ ULONG BufferSize,
    _Out_ GUID *Guid)
{
    SHA_CTX Context;
    GUID Namespace;
    ULONG Digest[5];

    PAGED_CODE();

    if (NamespaceGuid == NULL)
        return STATUS_INVALID_PARAMETER_1;

    if (Guid == NULL)
        return STATUS_INVALID_PARAMETER_4;

    if ((Buffer == NULL) && (BufferSize != 0))
        return STATUS_INVALID_PARAMETER_3;

    /* The namespace is hashed with its integer fields in network byte order */
    Namespace = *NamespaceGuid;
    Namespace.Data1 = RtlUlongByteSwap(Namespace.Data1);
    Namespace.Data2 = RtlUshortByteSwap(Namespace.Data2);
    Namespace.Data3 = RtlUshortByteSwap(Namespace.Data3);

    A_SHAInit(&Context);
    A_SHAUpdate(&Context, (const UCHAR *)&Namespace, sizeof(Namespace));
    A_SHAUpdate(&Context, Buffer, BufferSize);
    A_SHAFinal(&Context, Digest);

    /* The first 16 bytes of the digest are the GUID, also in network byte order */
    RtlCopyMemory(Guid, Digest, sizeof(*Guid));
    Guid->Data1 = RtlUlongByteSwap(Guid->Data1);
    Guid->Data2 = RtlUshortByteSwap(Guid->Data2);
    Guid->Data3 = RtlUshortByteSwap(Guid->Data3);

    /* Stamp version 5 and the RFC 4122 variant */
    Guid->Data3 = (Guid->Data3 & 0x0FFF) | 0x5000;
    Guid->Data4[0] = (Guid->Data4[0] & 0x3F) | 0x80;

    return STATUS_SUCCESS;
}

#if !defined(_M_IX86)
//
// Stub for architectures which don't have this implemented
//
VOID
FASTCALL
RtlPrefetchMemoryNonTemporal(IN PVOID Source,
                             IN SIZE_T Length)
{
    //
    // Do nothing
    //
    UNREFERENCED_PARAMETER(Source);
    UNREFERENCED_PARAMETER(Length);
}
#endif

VOID NTAPI
AVrfInternalHeapFreeNotification(PVOID AllocationBase, SIZE_T AllocationSize)
{
    /* Stub for linking against rtl */
}



/* EOF */
