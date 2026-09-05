/*
 * PROJECT:     ReactOS Hardware Abstraction Layer
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PCIe MMCONFIG (ECAM) window publication for the PnP arbiters
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */


/* INCLUDES *****************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ******************************************************************/

#define MCFG_SIGNATURE 0x4746434D            /* "MCFG" */
#define TAG_HAL_MCFG   'fcMH'

#include <pshpack1.h>
typedef struct _MCFG_ALLOCATION
{
    ULONGLONG BaseAddress;
    USHORT PciSegment;
    UCHAR StartBusNumber;
    UCHAR EndBusNumber;
    ULONG Reserved;
} MCFG_ALLOCATION, *PMCFG_ALLOCATION;

typedef struct _MCFG_TABLE
{
    DESCRIPTION_HEADER Header;
    ULONGLONG Reserved;
    MCFG_ALLOCATION Allocations[ANYSIZE_ARRAY];
} MCFG_TABLE, *PMCFG_TABLE;
#include <poppack.h>

/* FUNCTIONS ****************************************************************/

/**
 * @brief
 * Opens (creating if needed) the Arbiters\ReservedResources policy
 * key the MMCONFIG window is published under.
 *
 * @param[out] KeyHandle
 * Receives the opened key handle.
 *
 * @return
 * Returns STATUS_SUCCESS, or the open/create failure status.
 */
static
NTSTATUS
HalpOpenReservedResourcesKey(
    _Out_ PHANDLE KeyHandle)
{
    UNICODE_STRING KeyName;
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE ArbitersKey;
    NTSTATUS Status;

    PAGED_CODE();

    RtlInitUnicodeString(&KeyName,
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Arbiters");
    InitializeObjectAttributes(&ObjectAttributes, &KeyName,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwOpenKey(&ArbitersKey, KEY_READ | KEY_WRITE, &ObjectAttributes);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlInitUnicodeString(&KeyName, L"ReservedResources");
    InitializeObjectAttributes(&ObjectAttributes, &KeyName,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, ArbitersKey, NULL);
    Status = ZwCreateKey(KeyHandle, KEY_READ | KEY_WRITE, &ObjectAttributes,
                         0, NULL, REG_OPTION_NON_VOLATILE, NULL);

    ZwClose(ArbitersKey);
    return Status;
}

/**
 * @brief
 * Publishes this machine's MMCONFIG (ECAM) windows for the PnP
 * memory arbiter, or withdraws a stale publication when the
 * firmware no longer describes one.
 *
 * @remarks
 * Reads the ACPI MCFG table and serialises one memory window per
 * allocation entry - [Base + StartBus MB, Base + (EndBus+1) MB - 1],
 * one megabyte of configuration space per bus
 */
VOID
NTAPI
HalpPublishMmConfigRanges(VOID)
{
    UNICODE_STRING ValueName;
    PMCFG_TABLE Mcfg;
    PIO_RESOURCE_REQUIREMENTS_LIST ReqList = NULL;
    PIO_RESOURCE_LIST List;
    HANDLE KeyHandle;
    ULONG EntryCount = 0;
    ULONG Size = 0;
    ULONG Index;
    NTSTATUS Status;

    PAGED_CODE();

    if (!NT_SUCCESS(HalpOpenReservedResourcesKey(&KeyHandle)))
        return;

    RtlInitUnicodeString(&ValueName, L"MmConfigRange");

    /* Fetch and validate the MCFG table. */
    Mcfg = HalAcpiGetTable(NULL, MCFG_SIGNATURE);
    if (Mcfg != NULL &&
        Mcfg->Header.Length >= FIELD_OFFSET(MCFG_TABLE, Allocations) + sizeof(MCFG_ALLOCATION))
    {
        EntryCount = (Mcfg->Header.Length - FIELD_OFFSET(MCFG_TABLE, Allocations)) /
                     sizeof(MCFG_ALLOCATION);
    }

    if (EntryCount == 0)
    {
        /* No (valid) MCFG: withdraw any stale publication from a prior boot. */
        ZwDeleteValueKey(KeyHandle, &ValueName);
        ZwClose(KeyHandle);
        return;
    }

    /* Serialise one memory window per MCFG allocation entry. */
    Size = FIELD_OFFSET(IO_RESOURCE_REQUIREMENTS_LIST, List) +
           FIELD_OFFSET(IO_RESOURCE_LIST, Descriptors) +
           EntryCount * sizeof(IO_RESOURCE_DESCRIPTOR);

    ReqList = ExAllocatePoolWithTag(PagedPool, Size, TAG_HAL_MCFG);
    if (ReqList == NULL)
    {
        ZwClose(KeyHandle);
        return;
    }
    RtlZeroMemory(ReqList, Size);

    ReqList->ListSize = Size;
    ReqList->AlternativeLists = 1;

    List = &ReqList->List[0];
    List->Version = 1;
    List->Revision = 1;
    List->Count = EntryCount;

    for (Index = 0; Index < EntryCount; Index++)
    {
        PMCFG_ALLOCATION Entry = &Mcfg->Allocations[Index];
        PIO_RESOURCE_DESCRIPTOR Descriptor = &List->Descriptors[Index];

        /* One megabyte of configuration space per bus. */
        Descriptor->Type = CmResourceTypeMemory;
        Descriptor->u.Memory.MinimumAddress.QuadPart =
            Entry->BaseAddress + ((ULONGLONG)Entry->StartBusNumber << 20);
        Descriptor->u.Memory.MaximumAddress.QuadPart =
            Entry->BaseAddress + (((ULONGLONG)Entry->EndBusNumber + 1) << 20) - 1;

        DPRINT1("HAL: MMCONFIG segment %u buses %u-%u at 0x%I64x-0x%I64x\n",
                Entry->PciSegment, Entry->StartBusNumber, Entry->EndBusNumber,
                Descriptor->u.Memory.MinimumAddress.QuadPart,
                Descriptor->u.Memory.MaximumAddress.QuadPart);
    }

    Status = ZwSetValueKey(KeyHandle, &ValueName, 0,
                           REG_RESOURCE_REQUIREMENTS_LIST, ReqList, Size);
    if (!NT_SUCCESS(Status))
        DPRINT1("HAL: publishing MmConfigRange failed 0x%08lx\n", Status);

    ExFreePoolWithTag(ReqList, TAG_HAL_MCFG);
    ZwClose(KeyHandle);
}
