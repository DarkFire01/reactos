/*
 * PROJECT:     ReactOS NTFS driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Directory index walks and the caches behind them
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/**
* @name NtfsCreateMetadataStream
* @implemented
*
* Creates an internal stream file object, and the FCB behind it, so metadata
* can be read through the cache.
*
* @param Vcb
* Volume the stream belongs to
*
* @param Flags
* FCB_IS_MFT_STREAM or FCB_IS_INDEX_STREAM; tells the paging path what to read
*
* @param MftIndex
* File record the stream's attribute lives in
*
* @param Size
* Size of the attribute, in bytes
*
* @param StreamFcb
* Receives the new FCB. Its FileObject is the stream.
*
* @return
* STATUS_SUCCESS, or the failure that stopped us.
*/
NTSTATUS
NTAPI
NtfsCreateMetadataStream(
    _In_ PDEVICE_EXTENSION Vcb,
    _In_ ULONG Flags,
    _In_ ULONGLONG MftIndex,
    _In_ ULONGLONG Size,
    _Out_ PNTFS_FCB *StreamFcb)
{
    PNTFS_FCB Fcb;
    PFILE_OBJECT FileObject = NULL;
    NTSTATUS Status = STATUS_SUCCESS;

    *StreamFcb = NULL;

    Fcb = NtfsCreateFCB(NULL, NULL, Vcb);
    if (Fcb == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Fcb->Flags = Flags;
    Fcb->MFTIndex = MftIndex;
    Fcb->RefCount = 1;
    Fcb->RFCB.AllocationSize.QuadPart = Size;
    Fcb->RFCB.FileSize.QuadPart = Size;
    Fcb->RFCB.ValidDataLength.QuadPart = Size;
    Fcb->RFCB.IsFastIoPossible = FastIoIsNotPossible;

    _SEH2_TRY
    {
        /* No handle, so no cleanup IRP, and no CCB for close to free */
        FileObject = IoCreateStreamFileObjectLite(NULL, Vcb->StorageDevice);

        /* Takes over the VPB reference the call made, so closing gives it back */
        FileObject->Vpb = Vcb->StorageDevice->Vpb;
        FileObject->FsContext = Fcb;
        FileObject->FsContext2 = NULL;
        FileObject->SectionObjectPointer = &Fcb->SectionObjectPointers;
        FileObject->ReadAccess = TRUE;
        FileObject->WriteAccess = TRUE;
        Fcb->FileObject = FileObject;

        CcInitializeCacheMap(FileObject,
                             (PCC_FILE_SIZES)&Fcb->RFCB.AllocationSize,
                             FALSE,
                             &NtfsGlobalData->CacheMgrCallbacks,
                             Fcb);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    if (!NT_SUCCESS(Status))
    {
        if (FileObject != NULL)
        {
            FileObject->FsContext = NULL;
            ObDereferenceObject(FileObject);
        }

        NtfsDestroyFCB(Fcb);
        return Status;
    }

    Fcb->Flags |= FCB_CACHE_INITIALIZED;
    *StreamFcb = Fcb;

    return STATUS_SUCCESS;
}

/**
* @name NtfsCloseMetadataStream
* @implemented
*
* Tears down a stream made by NtfsCreateMetadataStream().
*
* @param StreamFcb
* The stream's FCB. Nothing may be reading through it any more.
*
* @remarks The FCB is only freed once Mm let go of the section, the same rule
* NtfsReleaseFCB() follows.
*/
VOID
NTAPI
NtfsCloseMetadataStream(
    _In_ PNTFS_FCB StreamFcb)
{
    LARGE_INTEGER Zero;

    Zero.QuadPart = 0;

    if (StreamFcb->FileObject != NULL)
    {
        CcUninitializeCacheMap(StreamFcb->FileObject, &Zero, NULL);
        ObDereferenceObject(StreamFcb->FileObject);
        StreamFcb->FileObject = NULL;
    }

    if (StreamFcb->SectionObjectPointers.DataSectionObject != NULL)
    {
        DPRINT1("Keeping metadata stream FCB %p: still mapped\n", StreamFcb);
        return;
    }

    NtfsDestroyFCB(StreamFcb);
}

/* EOF */
