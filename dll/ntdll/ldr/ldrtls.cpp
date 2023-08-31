
/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS NT User-Mode Library
 * FILE:            dll/ntdll/ldr/ldrinit.c
 * PURPOSE:         User-Mode Process/Thread Startup
 * PROGRAMMERS:     Alex Ionescu (alex@relsoft.net)
 *                  Aleksey Bragin (aleksey@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include <ntdll.h>
#include <compat_undoc.h>
#include <compatguid_undoc.h>

#define NDEBUG
#include <debug.h>


extern "C"
{
LIST_ENTRY LdrpTlsList;

RTL_BITMAP LdrpTlsBitmap;
ULONG LdrpActiveThreadCount = 0;
ULONG LdrpActualBitmapSize = 0;
TLS_RECLAIM_TABLE_ENTRY LdrpDelayedTlsReclaimTable[16];
}
RTL_SRWLOCK LdrpTlsLock = RTL_SRWLOCK_INIT;

#define LDR_HEX32_FMT "0x{:08X}"
#define LDR_HEX64_FMT "0x{:016X}"
#define LDR_HEXPTR_FMT "0x{:08X}"
/* FUNCTIONS *****************************************************************/

PVOID* LdrpGetNewTlsVector(IN ULONG TlsBitmapLength)
{
    auto* TlsVector = (PTLS_VECTOR)RtlAllocateHeap(
        RtlGetProcessHeap(),
        HEAP_ZERO_MEMORY,
        FIELD_OFFSET(TLS_VECTOR, ModuleTlsData[TlsBitmapLength])
    );

    if (!TlsVector)
        return NULL;

    TlsVector->Length = TlsBitmapLength;
    return TlsVector->ModuleTlsData;
}

NTSTATUS
LdrpGrowTlsBitmap(IN ULONG newLength)
{
    const ULONG AlignedLength = LDRP_BITMAP_CALC_ALIGN(newLength, LDRP_BITMAP_BITALIGN);

    const PULONG NewBitmapBuffer = (PULONG)RtlAllocateHeap(
        RtlGetProcessHeap(),
        HEAP_ZERO_MEMORY, // we will clear bits later
        AlignedLength * sizeof(ULONG)
    );

    if (!NewBitmapBuffer)
    {
        return STATUS_NO_MEMORY;
    }

    LdrpActualBitmapSize = AlignedLength;

    if (LdrpTlsBitmap.SizeOfBitMap > 0)
    {
        // Copy the contents of the previous buffer into the new one.
        RtlCopyMemory(
            NewBitmapBuffer,
            LdrpTlsBitmap.Buffer,
            LDRP_BITMAP_CALC_ALIGN(LdrpTlsBitmap.SizeOfBitMap, 8)
        );

        // Free the old buffer.
        RtlFreeHeap(RtlGetProcessHeap(), 0, LdrpTlsBitmap.Buffer);
    }

    // Reinitialize the bitmap as we've changed the buffer pointer.

    RtlInitializeBitMap(
        &LdrpTlsBitmap,
        NewBitmapBuffer,
        newLength
    );

    return STATUS_SUCCESS;
}

NTSTATUS
LdrpAcquireTlsIndex(
    OUT PULONG TlsIndex,
    OUT PBOOLEAN AllocatedBitmap
)
{
    ULONG Index;

    if (LdrpTlsBitmap.SizeOfBitMap > 0)
    {
        Index = RtlFindClearBitsAndSet(&LdrpTlsBitmap, 1, 0);

        // If we found space in the existing bitmap then there is no reason to
        // expand buffers, so we'll just return with the existing data.

        if (Index != 0xFFFFFFFF)
        {
            *TlsIndex = Index;
            *AllocatedBitmap = FALSE;

            return STATUS_SUCCESS;
        }
    }

    Index = LdrpTlsBitmap.SizeOfBitMap;

    const ULONG NewLength = LdrpTlsBitmap.SizeOfBitMap + TLS_BITMAP_GROW_INCREMENT;

    // Check if we need to grow the bitmap itself or if the bitmap still
    // has space
    if (LDRP_BITMAP_CALC_ALIGN(NewLength, LDRP_BITMAP_BITALIGN) > LdrpActualBitmapSize)
    {
        // We'll need to grow it.  Let's go do so now.

        NTSTATUS Status;
        if (!NT_SUCCESS(Status = LdrpGrowTlsBitmap(NewLength)))
            return Status;
    }
    else
    {
        LdrpTlsBitmap.SizeOfBitMap += TLS_BITMAP_GROW_INCREMENT;
    }

    RtlSetBit(&LdrpTlsBitmap, Index);

    *TlsIndex = Index;
    *AllocatedBitmap = TRUE;

    return STATUS_SUCCESS;
}

VOID
LdrpReleaseTlsIndex(IN ULONG TlsIndex)
{
    RtlClearBit(&LdrpTlsBitmap, TlsIndex);
}

NTSTATUS
LdrpAllocateTlsEntry(
    IN PIMAGE_TLS_DIRECTORY TlsDirectory,
    IN PLDR_DATA_TABLE_ENTRY ModuleEntry,
    OUT PULONG TlsIndex,
    OUT PBOOLEAN AllocatedBitmap OPTIONAL,
    OUT PTLS_ENTRY* TlsEntry
)
{
    PTLS_ENTRY Entry = NULL;
    NTSTATUS Status;

    ASSERT(TlsDirectory);

    __try
    {
        Entry = (PTLS_ENTRY)RtlAllocateHeap(RtlGetProcessHeap(), 0, sizeof(TLS_ENTRY));

        if (!Entry)
            _SEH2_YIELD(return STATUS_NO_MEMORY);

        Status = STATUS_SUCCESS;

        RtlCopyMemory(&Entry->TlsDirectory, TlsDirectory, sizeof(IMAGE_TLS_DIRECTORY));
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = GetExceptionCode();
    }
    _SEH2_END

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("TLS entry creation failed [0x%08lX], exiting...\n", Status);

        RtlFreeHeap(RtlGetProcessHeap(), 0, Entry);

        return Status;
    }

    // Validate that the TLS directory entry is sane.

    if (Entry->TlsDirectory.StartAddressOfRawData > Entry->TlsDirectory.EndAddressOfRawData)
    {
        if (ModuleEntry)
        {
            DPRINT1("TLS Directory of %ls has erroneous data range: [%lu, %lu]\n",
                    ModuleEntry->FullDllName.Buffer, TlsDirectory->StartAddressOfRawData,
                    TlsDirectory->EndAddressOfRawData);
        }
        else
        {
            DPRINT1("TLS Directory [UNKNOWN MODULE] has erroneous data range: [%lu, %lu]\n",
                    TlsDirectory->StartAddressOfRawData, TlsDirectory->EndAddressOfRawData);
        }

        RtlFreeHeap(RtlGetProcessHeap(), 0, Entry);

        return STATUS_INVALID_IMAGE_FORMAT;
    }

    Entry->ModuleEntry = ModuleEntry;

    // Insert the entry into our list.

    InsertTailList(&LdrpTlsList, &Entry->TlsEntryLinks);

    if (AllocatedBitmap)
    {
        Status = LdrpAcquireTlsIndex(TlsIndex, AllocatedBitmap);

        if (!NT_SUCCESS(Status))
        {
            DPRINT1("TLS index acquisition failed during entry creation [0x%08lX].\n", Status);

            RemoveEntryList(&Entry->TlsEntryLinks);

            RtlFreeHeap(RtlGetProcessHeap(), 0, Entry);

            return Status;
        }
    }
    else
    {
        *TlsIndex += 1;
    }

    // We reuse the 'Characteristics' field for the real TLS index.

    Entry->TlsDirectory.Characteristics = *TlsIndex;

    __try
    {
        *(PULONG)Entry->TlsDirectory.AddressOfIndex = *TlsIndex;
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = GetExceptionCode();
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("TLS directory index persistence failed during entry creation [0x%08lX].\n", Status);

        if (AllocatedBitmap)
        {
            LdrpReleaseTlsIndex(*TlsIndex);

            if (*AllocatedBitmap)
                LdrpTlsBitmap.SizeOfBitMap -= TLS_BITMAP_GROW_INCREMENT;
        }

        RemoveEntryList(&Entry->TlsEntryLinks);

        RtlFreeHeap(RtlGetProcessHeap(), 0, Entry);

        return Status;
    }

    if (TlsEntry)
        *TlsEntry = Entry;

    return STATUS_SUCCESS;
}

PTLS_ENTRY
FASTCALL
LdrpFindTlsEntry(IN PLDR_DATA_TABLE_ENTRY ModuleEntry)
{
    PTLS_ENTRY TlsEntry;

    PLIST_ENTRY ListHead = &LdrpTlsList;

    for (TlsEntry = CONTAINING_RECORD(LdrpTlsList.Flink, TLS_ENTRY, TlsEntryLinks);
         &TlsEntry->TlsEntryLinks != ListHead;
         TlsEntry = CONTAINING_RECORD(TlsEntry->TlsEntryLinks.Flink, TLS_ENTRY, TlsEntryLinks))
    {
        if (TlsEntry->ModuleEntry == ModuleEntry)
            return TlsEntry;
    }

    return 0;
}

NTSTATUS
NTAPI
LdrpReleaseTlsEntry(IN PLDR_DATA_TABLE_ENTRY ModuleEntry, OUT PTLS_ENTRY* FoundTlsEntry OPTIONAL)
{
    if (!FoundTlsEntry)
        RtlAcquireSRWLockExclusive(&LdrpTlsLock);

    // Find the corresponding TLS_ENTRY for this module entry.

    PTLS_ENTRY TlsEntry = LdrpFindTlsEntry(ModuleEntry);

    if (!TlsEntry)
    {
        if (!FoundTlsEntry)
            RtlReleaseSRWLockExclusive(&LdrpTlsLock);
        return STATUS_NOT_FOUND;
    }

    // Remove it from the global list of outstanding TLS entries.

    RemoveEntryList(&TlsEntry->TlsEntryLinks);

    // Deallocate the TLS index.

    LdrpReleaseTlsIndex(TlsEntry->TlsDirectory.Characteristics);

    if (!FoundTlsEntry)
        RtlReleaseSRWLockExclusive(&LdrpTlsLock);

    // Deallocate the TLS_ENTRY object itself, unless FoundTlsEntry is non-NULL
    // Call from HandleTlsData does that, because of SRW lock already being held.
    if (FoundTlsEntry)
        *FoundTlsEntry = TlsEntry;
    else
        RtlFreeHeap(RtlGetProcessHeap(), 0, TlsEntry);

    // We're done.

    return STATUS_SUCCESS;
}

VOID
LdrpQueueDeferredTlsData(
    IN OUT PVOID TlsVector,
    IN OUT PVOID ThreadId
)
{
    const PTLS_RECLAIM_TABLE_ENTRY ReclaimEntry =
        &LdrpDelayedTlsReclaimTable[((ULONG_PTR)(ThreadId) >> 2) & 0xF];

    const PTLS_VECTOR RealTlsVector = CONTAINING_RECORD(
        TlsVector,
        TLS_VECTOR,
        ModuleTlsData
    );

    RealTlsVector->ThreadId = ThreadId;

#if 1
    RtlAcquireSRWLockExclusive(&ReclaimEntry->Lock);
#endif

    RealTlsVector->PreviousDeferredTlsVector = ReclaimEntry->TlsVector;
    ReclaimEntry->TlsVector = RealTlsVector;

#if 1
    RtlReleaseSRWLockExclusive(&ReclaimEntry->Lock);
#endif
}

BOOL
LdrpCleanupThreadTlsData(VOID)
{
    BOOL Result = TRUE;
    PTLS_VECTOR TargetReclaimTlsVector = NULL;
    PTLS_VECTOR PreviousReclaimVector = NULL;
    const HANDLE CurrentThreadHandle = NtCurrentTeb()->RealClientId.UniqueThread;
    const PTLS_RECLAIM_TABLE_ENTRY FoundReclaimEntry =
        &LdrpDelayedTlsReclaimTable[((ULONG_PTR)CurrentThreadHandle >> 2) & 0xF];
    PTLS_VECTOR CurrentReclaimVector = FoundReclaimEntry->TlsVector;

#if 1
    RtlAcquireSRWLockExclusive(&FoundReclaimEntry->Lock);
#endif

    while (CurrentReclaimVector)
    {
        const PTLS_VECTOR NextReclaimVector = CurrentReclaimVector->PreviousDeferredTlsVector;

        if (CurrentReclaimVector->ThreadId == CurrentThreadHandle)
        {
            if (PreviousReclaimVector)
                PreviousReclaimVector->PreviousDeferredTlsVector = NextReclaimVector;
            else
                FoundReclaimEntry->TlsVector = NextReclaimVector;

            CurrentReclaimVector->PreviousDeferredTlsVector = TargetReclaimTlsVector;
            TargetReclaimTlsVector = CurrentReclaimVector;
            CurrentReclaimVector = PreviousReclaimVector;
        }

        PreviousReclaimVector = CurrentReclaimVector;
        CurrentReclaimVector = NextReclaimVector;
    }

#if 1
    RtlReleaseSRWLockExclusive(&FoundReclaimEntry->Lock);
#endif

    while (TargetReclaimTlsVector)
    {
        const PTLS_VECTOR NextReclaimTlsVector = TargetReclaimTlsVector->PreviousDeferredTlsVector;

        Result = RtlFreeHeap(RtlGetProcessHeap(), 0, TargetReclaimTlsVector);
        TargetReclaimTlsVector = NextReclaimTlsVector;
    }

    return Result;
}

NTSTATUS
NTAPI
LdrpInitializeTls(VOID)
{
    const PLIST_ENTRY ListHead = &NtCurrentPeb()->Ldr->InLoadOrderModuleList;
    ULONG TlsIndex = 0;
    NTSTATUS Status;

    /* Initialize the TLS List */
    InitializeListHead(&LdrpTlsList);

    /* Loop all the modules */
    for (PLIST_ENTRY NextEntry = ListHead->Flink; ListHead != NextEntry; NextEntry = NextEntry->Flink)
    {
        ULONG Size;

        /* Get the entry */
        PLDR_DATA_TABLE_ENTRY Module = CONTAINING_RECORD(NextEntry, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);

        /* Get the TLS directory */
        auto* TlsDirectory = (PIMAGE_TLS_DIRECTORY)RtlImageDirectoryEntryToData(Module->DllBase,
                                                                               TRUE,
                                                                               IMAGE_DIRECTORY_ENTRY_TLS,
                                                                               &Size);

        /* Check if we have a directory */
        if (!TlsDirectory || !Size) continue;

        /* Show debug message */
        if (1)
        {
            DPRINT1("LDR: Tls Found in %wZ at " LDR_HEXPTR_FMT "\n",
                    &Module->BaseDllName,
                    TlsDirectory);
        }

        /* Allocate an entry */
        Status = LdrpAllocateTlsEntry(TlsDirectory, Module, &TlsIndex, NULL, NULL);
        if (!NT_SUCCESS(Status)) return Status;

        /* Lock the DLL and mark it for TLS Usage */
        Module->LoadCount = LDR_LOADCOUNT_MAX;
        Module->TlsIndex = -1;
    }

    if (!TlsIndex)
    {
        RtlInitializeBitMap(&LdrpTlsBitmap, NULL, 0);
    }
    else
    {
        // First-time equivalent of LdrpAcquireTlsIndex

        const ULONG BitMapSize = TlsIndex + TLS_BITMAP_GROW_INCREMENT;

        if (!NT_SUCCESS(Status = LdrpGrowTlsBitmap(BitMapSize)))
            return Status;

        RtlSetBits(&LdrpTlsBitmap, 0, TlsIndex);
    }

    /* Done setting up TLS, allocate entries */
    return LdrpAllocateTls();
}

NTSTATUS
NTAPI
LdrpAllocateTls(VOID)
{
    const PLIST_ENTRY ListHead = &LdrpTlsList;
    PLIST_ENTRY NextEntry;
    PVOID* TlsVector;

    RtlAcquireSRWLockShared(&LdrpTlsLock);

    /* Check if we have any entries */
    if (!LdrpTlsBitmap.SizeOfBitMap)
        goto success_exit;

    /* Allocate the vector array */
    TlsVector = LdrpGetNewTlsVector(LdrpTlsBitmap.SizeOfBitMap);
    if (!TlsVector)
    {
        RtlReleaseSRWLockShared(&LdrpTlsLock);
        return STATUS_NO_MEMORY;
    }

    /* Loop the TLS Array */
    for (NextEntry = ListHead->Flink; NextEntry != ListHead; NextEntry = NextEntry->Flink)
    {
        PTLS_ENTRY TlsData;
        SIZE_T TlsDataSize;

        /* Get the entry */
        TlsData = CONTAINING_RECORD(NextEntry, TLS_ENTRY, TlsEntryLinks);

        /* Allocate this vector */
        TlsDataSize = TlsData->TlsDirectory.EndAddressOfRawData -
            TlsData->TlsDirectory.StartAddressOfRawData;
        TlsVector[TlsData->TlsDirectory.Characteristics] = RtlAllocateHeap(RtlGetProcessHeap(),
                                                                           0,
                                                                           TlsDataSize);
        if (!TlsVector[TlsData->TlsDirectory.Characteristics])
        {
            /* Out of memory */

            ULONG i = 0;
            for (; i < LdrpTlsBitmap.SizeOfBitMap; ++i)
                if (TlsVector[i])
                    RtlFreeHeap(RtlGetProcessHeap(), 0, TlsVector[i]);

            RtlReleaseSRWLockShared(&LdrpTlsLock);
            return STATUS_NO_MEMORY;
        }

        /* Show debug message */
        if (1)
        {
            DPRINT1("LDR: TlsVector " LDR_HEXPTR_FMT " Index %lu = " LDR_HEXPTR_FMT " copied from %x to " LDR_HEXPTR_FMT "\n",
                    TlsVector,
                    TlsData->TlsDirectory.Characteristics,
                    &TlsVector[TlsData->TlsDirectory.Characteristics],
                    TlsData->TlsDirectory.StartAddressOfRawData,
                    TlsVector[TlsData->TlsDirectory.Characteristics]);
        }

        /* Copy the data */
        RtlCopyMemory(TlsVector[TlsData->TlsDirectory.Characteristics],
                      (PVOID)TlsData->TlsDirectory.StartAddressOfRawData,
                      TlsDataSize);
    }

    NtCurrentTeb()->ThreadLocalStoragePointer = TlsVector;

success_exit:
    InterlockedIncrement((PLONG)&LdrpActiveThreadCount);
    RtlReleaseSRWLockShared(&LdrpTlsLock);
    /* Done */
    return STATUS_SUCCESS;
}

VOID
NTAPI
LdrpFreeTls(VOID)
{
    PTEB Teb = NtCurrentTeb();

    RtlAcquireSRWLockShared(&LdrpTlsLock);

    /* Get a pointer to the vector array */
    PVOID* TlsVector = (PVOID*)Teb->ThreadLocalStoragePointer;

    InterlockedDecrement((PLONG)&LdrpActiveThreadCount);
    Teb->ThreadLocalStoragePointer = NULL;

    RtlReleaseSRWLockShared(&LdrpTlsLock);

    if (TlsVector)
    {
        /* Loop through it */
        for (ULONG i = 0; i < LdrpTlsBitmap.SizeOfBitMap; ++i)
            if (TlsVector[i])
                RtlFreeHeap(RtlGetProcessHeap(), 0, TlsVector[i]);

        auto* RealTlsVector = CONTAINING_RECORD(TlsVector, TLS_VECTOR, ModuleTlsData);

        /* Free the array itself */
        RtlFreeHeap(RtlGetProcessHeap(), 0, RealTlsVector);
    }

    LdrpCleanupThreadTlsData();
}


VOID
NTAPI
LdrpCallTlsInitializers(IN PLDR_DATA_TABLE_ENTRY Module,
                        IN ULONG Reason)
{
    RtlAcquireSRWLockShared(&LdrpTlsLock);

    /* Get the TLS Directory */
    const PTLS_ENTRY TlsEntry = LdrpFindTlsEntry(Module);

    RtlReleaseSRWLockShared(&LdrpTlsLock);

    /* Protect against invalid pointers */
    __try
    {
        /* Make sure it's valid */
        if (TlsEntry)
        {
            /* Get the array */
            auto* Array = (PIMAGE_TLS_CALLBACK *)TlsEntry->TlsDirectory.AddressOfCallBacks;
            if (Array)
            {
                /* Display debug */
                if (1)
                {
                    DPRINT1("LDR: Tls Callbacks Found. Imagebase " LDR_HEXPTR_FMT " Tls " LDR_HEXPTR_FMT " CallBacks " LDR_HEXPTR_FMT "\n",
                            Module->DllBase, TlsEntry->TlsDirectory, Array);
                }

                /* Loop the array */
                while (*Array)
                {
                    /* Get the TLS Entrypoint */
                    const PIMAGE_TLS_CALLBACK Callback = *Array++;

                    /* Display debug */
                    if (1)
                    {
                        DPRINT1("LDR: Calling Tls Callback Imagebase " LDR_HEXPTR_FMT " Function " LDR_HEXPTR_FMT "\n",
                                Module->DllBase, Callback);
                    }

                    /* Call it */
                    LdrpCallInitRoutine((PDLL_INIT_ROUTINE)Callback,
                                        Module->DllBase,
                                        Reason,
                                        NULL);
                }
            }
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        DPRINT1("LDR: Exception 0x%x during Tls Callback(%u) for %wZ\n",
                GetExceptionCode(), Reason, &Module->BaseDllName);
    }
}


/* EOF */




#define SWAPD(x) x
#define SWAPW(x) x
NTSTATUS
NTAPI
RtlpImageDirectoryEntryToData32(
    PVOID BaseAddress,
    BOOLEAN MappedAsImage,
    USHORT Directory,
    PULONG Size,
    PVOID* Section,
    PIMAGE_NT_HEADERS32 NtHeader)
{
    ULONG Va;
    PVOID result;

    if (Directory >= SWAPD(NtHeader->OptionalHeader.NumberOfRvaAndSizes))
        return STATUS_INVALID_PARAMETER;

    Va = SWAPD(NtHeader->OptionalHeader.DataDirectory[Directory].VirtualAddress);
    if (Va == 0)
        return STATUS_NOT_IMPLEMENTED;

    *Size = SWAPD(NtHeader->OptionalHeader.DataDirectory[Directory].Size);

    if (MappedAsImage || Va < SWAPD(NtHeader->OptionalHeader.SizeOfHeaders))
    {
        *Section = (PVOID)((ULONG_PTR)BaseAddress + Va);
        return STATUS_SUCCESS;
    }

    /* Image mapped as ordinary file, we must find raw pointer */
    result = RtlImageRvaToVa((PIMAGE_NT_HEADERS)NtHeader, BaseAddress, Va, NULL);
    *Section = result;
    return result ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
RtlpImageDirectoryEntryToData64(
    PVOID BaseAddress,
    BOOLEAN MappedAsImage,
    USHORT Directory,
    PULONG Size,
    PVOID* Section,
    PIMAGE_NT_HEADERS64 NtHeader)
{
    ULONG Va;
    PVOID result;

    if (Directory >= SWAPD(NtHeader->OptionalHeader.NumberOfRvaAndSizes))
        return STATUS_INVALID_PARAMETER;

    Va = SWAPD(NtHeader->OptionalHeader.DataDirectory[Directory].VirtualAddress);
    if (Va == 0)
        return STATUS_NOT_IMPLEMENTED;

    *Size = SWAPD(NtHeader->OptionalHeader.DataDirectory[Directory].Size);

    if (MappedAsImage || Va < SWAPD(NtHeader->OptionalHeader.SizeOfHeaders))
    {
        *Section = (PVOID)((ULONG_PTR)BaseAddress + Va);
        return STATUS_SUCCESS;
    }

    /* Image mapped as ordinary file, we must find raw pointer */
    result = RtlImageRvaToVa((PIMAGE_NT_HEADERS)NtHeader, BaseAddress, Va, NULL);
    *Section = result;
    return result ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

NTSTATUS
NTAPI
RtlImageDirectoryEntryToDataEx(
    PVOID BaseAddress,
    BOOLEAN MappedAsImage,
    USHORT Directory,
    PULONG Size,
    OUT PVOID* Section)
{
    PIMAGE_NT_HEADERS NtHeader;
    NTSTATUS Status;

    if ((ULONG_PTR)BaseAddress & 3)
    {
        /* Magic flag for non-mapped images. */
        if ((ULONG_PTR)BaseAddress & 1)
            MappedAsImage = FALSE;
        BaseAddress = (PVOID)((ULONG_PTR)BaseAddress & ~3);
    }

    Status = RtlImageNtHeaderEx(
        RTL_IMAGE_NT_HEADER_EX_FLAG_NO_RANGE_CHECK,
        BaseAddress,
        0,
        &NtHeader);

    if (NtHeader)
    {
        if (NtHeader->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
            Status = RtlpImageDirectoryEntryToData32(BaseAddress, MappedAsImage, Directory, Size, Section, (PIMAGE_NT_HEADERS32)NtHeader);
        else if (NtHeader->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            Status = RtlpImageDirectoryEntryToData64(BaseAddress, MappedAsImage, Directory, Size, Section, (PIMAGE_NT_HEADERS64)NtHeader);
        else
            Status = STATUS_INVALID_PARAMETER;
    }

    return Status;
}


NTSTATUS
NTAPI
LdrpHandleTlsData(IN OUT PLDR_DATA_TABLE_ENTRY ModuleEntry)
{
    PIMAGE_TLS_DIRECTORY TlsDirectory;
    ULONG DirectorySize;
    PPROCESS_TLS_INFORMATION TlsInfo;
    PTLS_ENTRY PendingReleaseTlsEntry = NULL;
    BOOLEAN AllocatedBitmap = FALSE, AllocatedTlsEntry = FALSE;
    NTSTATUS Status;

    if (LdrpActiveThreadCount == 0)
        return STATUS_SUCCESS;

    // Discover the TLS directory address for this module.

    Status = RtlImageDirectoryEntryToDataEx(
        ModuleEntry->DllBase,
        TRUE,
        IMAGE_DIRECTORY_ENTRY_TLS,
        &DirectorySize,
        (PVOID*)&TlsDirectory
    );

    // If we've got no TLS directory then we're done.

    if (!TlsDirectory || !DirectorySize || !NT_SUCCESS(Status))
        return STATUS_SUCCESS;

    RtlAcquireSRWLockExclusive(&LdrpTlsLock);

    // We'll be using the process heap.

    // We've got an optimization for one active thread, which is the case for
    // traditional static-link DLLs that use __declspec(thread).

    // Allocate memory for our thread data block.

    const LONG TlsInfoSize = FIELD_OFFSET(PROCESS_TLS_INFORMATION, ThreadData[LdrpActiveThreadCount]);
    TlsInfo = (PPROCESS_TLS_INFORMATION)RtlAllocateHeap(
        RtlGetProcessHeap(),
        0,
        TlsInfoSize
    );

    if (!TlsInfo)
    {
        RtlReleaseSRWLockExclusive(&LdrpTlsLock);
        return STATUS_NO_MEMORY;
    }

    do
    {
        ULONG TlsIndex;
        SIZE_T TlsRawDataLength;
        PTLS_ENTRY TlsEntry;
        ULONG ThreadIndex;
        ULONG ThreadsCleanedUp = 0;

        // Allocate a TLS index (or a new TLS bitmap).

        ULONG TlsBitmapLength = LdrpTlsBitmap.SizeOfBitMap;

        Status = LdrpAllocateTlsEntry(
            TlsDirectory,
            ModuleEntry,
            &TlsIndex,
            &AllocatedBitmap,
            &TlsEntry
        );

        if (!NT_SUCCESS(Status))
        {
            DPRINT1("TLS entry allocation failed [%#X], aborting...\n", Status);
            break;
        }

        AllocatedTlsEntry = TRUE;

        TlsInfo->ThreadDataCount = LdrpActiveThreadCount;

        if (AllocatedBitmap)
        {
            TlsInfo->OperationType = ProcessTlsReplaceVector;
            TlsInfo->TlsVectorLength = TlsBitmapLength;

            TlsBitmapLength = LdrpTlsBitmap.SizeOfBitMap;
        }
        else
        {
            TlsInfo->OperationType = ProcessTlsReplaceIndex;
            TlsInfo->TlsIndex = TlsIndex;
        }

        Status = STATUS_SUCCESS;

        // Calculate the size of the raw TLS data for this module.

        TlsRawDataLength = TlsEntry->TlsDirectory.EndAddressOfRawData -
            TlsEntry->TlsDirectory.StartAddressOfRawData;

        // Prepare data for each running thread.

        for (ThreadIndex = 0; ThreadIndex < TlsInfo->ThreadDataCount; ThreadIndex++)
        {
            const PVOID TlsMemoryBlock = RtlAllocateHeap(
                RtlGetProcessHeap(),
                0,
                TlsRawDataLength
            );

            if (!TlsMemoryBlock)
            {
                Status = STATUS_NO_MEMORY;
                break;
            }

            // Copy the initializer raw data into the newly allocated TLS memory block for thread #ThreadIndex.

            __try
            {
                RtlCopyMemory(
                    TlsMemoryBlock,
                    (PVOID)TlsEntry->TlsDirectory.StartAddressOfRawData,
                    TlsRawDataLength
                );
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                Status = GetExceptionCode();
            }
            _SEH2_END

            if (!NT_SUCCESS(Status))
            {
                DPRINT1("RtlCopyMemory failed [%#X], exiting...\n", Status);

                RtlFreeHeap(RtlGetProcessHeap(), 0, TlsMemoryBlock);

                break;
            }

            if (AllocatedBitmap)
            {
                PVOID* TlsVector = (PVOID*)LdrpGetNewTlsVector(TlsBitmapLength);

                if (!TlsVector)
                {
                    RtlFreeHeap(RtlGetProcessHeap(), 0, TlsMemoryBlock);

                    if (NT_SUCCESS(Status))
                        Status = STATUS_NO_MEMORY;

                    break;
                }

                TlsVector[TlsIndex] = TlsMemoryBlock;

                TlsInfo->ThreadData[ThreadIndex].TlsVector = TlsVector;
            }
            else
            {
                TlsInfo->ThreadData[ThreadIndex].TlsModulePointer = TlsMemoryBlock;
            }

            TlsInfo->ThreadData[ThreadIndex].Flags = 0;
        }

        if (!NT_SUCCESS(Status))
            break;

        TlsInfo->Reserved = 0;

        ASSERT(ThreadIndex == TlsInfo->ThreadDataCount);
        ASSERT(TlsInfoSize == FIELD_OFFSET(PROCESS_TLS_INFORMATION, ThreadData[TlsInfo->ThreadDataCount]));

        // Perform the actual work of swapping the thread TLS data.

        Status = NtSetInformationProcess(
            NtCurrentProcess(),
            ProcessTlsInformation,
            TlsInfo,
            TlsInfoSize
        );

        // Let's handle each thread that we replaced, as the
        // ProcessTlsInformation call fills our buffer with the old data
        // after performing a swap.

        while (ThreadIndex-- > 0)
        {
            const PTHREAD_TLS_INFORMATION ResultTlsInformation = &TlsInfo->ThreadData[ThreadIndex];

            if (ResultTlsInformation->Flags & 0x1)
            {
                // Failure in kernel mode, potential TLS memory leak
                DPRINT1("ResultTlsInformation->Flags [%#x] has failure bit set, status %#x.\n",
                    ResultTlsInformation->Flags, Status);
                continue;
            }

            // Same as ThreadTlsData->TlsModulePointer (union)
            if (!ResultTlsInformation->TlsVector)
                continue;

            // Success, schedule TLS block deletion on thread exit
            if ((ResultTlsInformation->Flags & 0x2) && AllocatedBitmap)
            {
                LdrpQueueDeferredTlsData(
                    ResultTlsInformation->TlsVector,
                    ResultTlsInformation->ThreadId
                );

                continue;
            }

            if (!ResultTlsInformation->Flags)
            {
                // Thread disposed (not iterated through using PsGetNextProcessThread
                ThreadsCleanedUp++;

                if (AllocatedBitmap)
                {
                    // Free the old TLS memory block, pointed to by the item in TLS_VECTOR
                    RtlFreeHeap(
                        RtlGetProcessHeap(),
                        0,
                        ResultTlsInformation->TlsVector[TlsIndex]
                    );

                    // Free the whole TLS_VECTOR allocated with LdrpGetNewTlsVector above
                    RtlFreeHeap(
                        RtlGetProcessHeap(),
                        0,
                        CONTAINING_RECORD(ResultTlsInformation->TlsVector, TLS_VECTOR, ModuleTlsData)
                    );
                }
            }

            if (!AllocatedBitmap)
            {
                // Free the old TLS memory block, pointed to by the TlsModulePointer
                RtlFreeHeap(
                    RtlGetProcessHeap(),
                    0,
                    ResultTlsInformation->TlsModulePointer
                );
            }
        }

        if (NT_SUCCESS(Status) && (ThreadsCleanedUp > 0))
        {
            DPRINT1("TLS Thread Post-Shutdown: success [%#X], decrementing LdrpActiveThreadCount: (%u)-(%u).\n",
                Status, LdrpActiveThreadCount, ThreadsCleanedUp);

            InterlockedExchangeAdd((PLONG)&LdrpActiveThreadCount, -((LONG)ThreadsCleanedUp));
        }
    }
    while (FALSE);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("TLS memory block swap failure [%#X], rolling back...\n", Status);

        if (AllocatedTlsEntry)
        {
            if (!NT_SUCCESS(LdrpReleaseTlsEntry(ModuleEntry, &PendingReleaseTlsEntry)))
                DPRINT1("TLS rollback memory release failure [%#X].\n");
        }

        if (AllocatedBitmap)
        {
            LdrpTlsBitmap.SizeOfBitMap -= TLS_BITMAP_GROW_INCREMENT;
        }
    }
    else
    {
        ModuleEntry->TlsIndex = -1; // just nonzero would be sufficient, but let's mimic the original
        Status = STATUS_SUCCESS;
    }

    RtlReleaseSRWLockExclusive(&LdrpTlsLock);

    RtlFreeHeap(RtlGetProcessHeap(), 0, TlsInfo);

    if (PendingReleaseTlsEntry)
        RtlFreeHeap(RtlGetProcessHeap(), 0, PendingReleaseTlsEntry);

    return Status;
}
