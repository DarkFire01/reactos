/*
 * PROJECT:     ReactOS Kernel32
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Process and thread attribute lists
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The attribute list an extended STARTUPINFOEX carries. It is an opaque blob to
 * the caller: it is allocated by the caller with the size we report from
 * InitializeProcThreadAttributeList(), filled in through
 * UpdateProcThreadAttribute() and handed to CreateProcess() with
 * EXTENDED_STARTUPINFO_PRESENT.
 *
 * The layout matches the one used by kernelbase, so that a list built here
 * stays readable if process creation ever starts honouring these attributes.
 */

#include <k32.h>

#define NDEBUG
#include <debug.h>

/* TYPES **********************************************************************/

struct proc_thread_attr
{
    DWORD_PTR attr;
    SIZE_T size;
    PVOID value;
};

struct _PROC_THREAD_ATTRIBUTE_LIST
{
    DWORD mask;  /* Bitmask of the attributes in the list */
    DWORD size;  /* Maximum number of attributes the list can hold */
    DWORD count; /* Number of attributes in the list */
    DWORD pad;
    DWORD_PTR unk;
    struct proc_thread_attr attrs[1];
};

/* PRIVATE FUNCTIONS **********************************************************/

static
DWORD
ValidateProcThreadAttribute(
    _In_ DWORD_PTR Attribute,
    _In_ SIZE_T Size)
{
    switch (Attribute)
    {
        case PROC_THREAD_ATTRIBUTE_PARENT_PROCESS:
            if (Size != sizeof(HANDLE)) return ERROR_BAD_LENGTH;
            break;

        case PROC_THREAD_ATTRIBUTE_HANDLE_LIST:
            if ((Size / sizeof(HANDLE)) * sizeof(HANDLE) != Size) return ERROR_BAD_LENGTH;
            break;

        case PROC_THREAD_ATTRIBUTE_IDEAL_PROCESSOR:
            if (Size != sizeof(PROCESSOR_NUMBER)) return ERROR_BAD_LENGTH;
            break;

        case PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY:
            if (Size != sizeof(DWORD) &&
                Size != sizeof(DWORD64) &&
                Size != sizeof(DWORD64) * 2)
            {
                return ERROR_BAD_LENGTH;
            }
            break;

        default:
            DPRINT1("Unhandled process/thread attribute %Iu\n",
                    Attribute & PROC_THREAD_ATTRIBUTE_NUMBER);
            return ERROR_NOT_SUPPORTED;
    }

    return ERROR_SUCCESS;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/*
 * @implemented
 */
BOOL
WINAPI
InitializeProcThreadAttributeList(
    _Out_writes_bytes_opt_(*lpSize) LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList,
    _In_ DWORD dwAttributeCount,
    _Reserved_ DWORD dwFlags,
    _Inout_ PSIZE_T lpSize)
{
    SIZE_T Needed;
    BOOL Ret = FALSE;

    if (lpSize == NULL || dwFlags != 0)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    Needed = FIELD_OFFSET(struct _PROC_THREAD_ATTRIBUTE_LIST, attrs[dwAttributeCount]);

    if (lpAttributeList != NULL && *lpSize >= Needed)
    {
        struct _PROC_THREAD_ATTRIBUTE_LIST *List = (PVOID)lpAttributeList;

        List->mask = 0;
        List->size = dwAttributeCount;
        List->count = 0;
        List->pad = 0;
        List->unk = 0;
        Ret = TRUE;
    }
    else
    {
        /* This is also how a caller asks for the size it has to allocate */
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
    }

    *lpSize = Needed;
    return Ret;
}

/*
 * @implemented
 */
BOOL
WINAPI
UpdateProcThreadAttribute(
    _Inout_ LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList,
    _In_ DWORD dwFlags,
    _In_ DWORD_PTR Attribute,
    _In_reads_bytes_opt_(cbSize) PVOID lpValue,
    _In_ SIZE_T cbSize,
    _Out_writes_bytes_opt_(cbSize) PVOID lpPreviousValue,
    _In_opt_ PSIZE_T lpReturnSize)
{
    struct _PROC_THREAD_ATTRIBUTE_LIST *List = (PVOID)lpAttributeList;
    struct proc_thread_attr *Entry;
    DWORD Mask, Error;

    UNREFERENCED_PARAMETER(dwFlags);
    UNREFERENCED_PARAMETER(lpPreviousValue);
    UNREFERENCED_PARAMETER(lpReturnSize);

    if (List == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (List->count >= List->size)
    {
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }

    Error = ValidateProcThreadAttribute(Attribute, cbSize);
    if (Error != ERROR_SUCCESS)
    {
        SetLastError(Error);
        return FALSE;
    }

    /* Each attribute may only be set once */
    Mask = 1 << (Attribute & PROC_THREAD_ATTRIBUTE_NUMBER);
    if (List->mask & Mask)
    {
        SetLastError(ERROR_OBJECT_NAME_EXISTS);
        return FALSE;
    }
    List->mask |= Mask;

    Entry = &List->attrs[List->count];
    Entry->attr = Attribute;
    Entry->size = cbSize;
    Entry->value = lpValue;
    List->count++;

    return TRUE;
}

/*
 * @implemented
 */
VOID
WINAPI
DeleteProcThreadAttributeList(
    _Inout_ LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList)
{
    /* The list is allocated by the caller, so there is nothing to free */
    UNREFERENCED_PARAMETER(lpAttributeList);
}

/* EOF */
