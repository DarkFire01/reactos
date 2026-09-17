/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/wmi/smbios.c
 * PURPOSE:         I/O Windows Management Instrumentation (WMI) Support
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#include <wmiguid.h>
#include <wmidata.h>
#include <wmistr.h>

#include "wmip.h"

#define NDEBUG
#include <debug.h>


/* FUNCTIONS *****************************************************************/

_At_(*OutTableData, __drv_allocatesMem(Mem))
NTSTATUS
NTAPI
WmipGetRawSMBiosTableData(
    _Outptr_opt_result_buffer_(*OutDataSize) PVOID *OutTableData,
    _Out_ PULONG OutDataSize)
{
    static const ULONG HeaderSize = FIELD_OFFSET(MSSmBios_RawSMBiosTables, SMBiosData);
    MSSmBios_RawSMBiosTables BiosTablesHeader;
    PVOID BiosTables;

    /* The structures were taken down while the firmware memory was still ours */
    if (ExpSmbiosTable.TableData == NULL)
    {
        DPRINT1("The firmware described no SMBIOS structures\n");
        return STATUS_NOT_FOUND;
    }

    RtlZeroMemory(&BiosTablesHeader, sizeof(BiosTablesHeader));
    BiosTablesHeader.Used20CallingMethod = 0;
    BiosTablesHeader.SmbiosMajorVersion = ExpSmbiosTable.MajorVersion;
    BiosTablesHeader.SmbiosMinorVersion = ExpSmbiosTable.MinorVersion;
    BiosTablesHeader.DmiRevision = ExpSmbiosTable.DmiRevision;
    BiosTablesHeader.Size = ExpSmbiosTable.TableLength;

    /* Check if the caller asked for the buffer */
    if (OutTableData != NULL)
    {
        /* Allocate a buffer for the result */
        BiosTables = ExAllocatePoolWithTag(PagedPool,
                                           HeaderSize + ExpSmbiosTable.TableLength,
                                           TAG_SMBIOS);
        if (BiosTables == NULL)
        {
            DPRINT1("Failed to allocate %lu bytes for the SMBIOS table\n",
                    HeaderSize + ExpSmbiosTable.TableLength);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        /* Copy the header, then the structures behind it */
        RtlCopyMemory(BiosTables, &BiosTablesHeader, HeaderSize);
        RtlCopyMemory((PUCHAR)BiosTables + HeaderSize,
                      ExpSmbiosTable.TableData,
                      ExpSmbiosTable.TableLength);

        *OutTableData = BiosTables;
    }

    *OutDataSize = HeaderSize + ExpSmbiosTable.TableLength;
    return STATUS_SUCCESS;
}


NTSTATUS
NTAPI
WmipQueryRawSMBiosTables(
    _Inout_ ULONG *InOutBufferSize,
    _Out_opt_ PVOID OutBuffer)
{
    NTSTATUS Status;
    PVOID TableData = NULL;
    ULONG TableSize, ResultSize;
    PWNODE_ALL_DATA AllData;

    /* Get the table data */
    Status = WmipGetRawSMBiosTableData(OutBuffer ? &TableData : NULL, &TableSize);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WmipGetRawSMBiosTableData failed: 0x%08lx\n", Status);
        return Status;
    }

    ResultSize = sizeof(WNODE_ALL_DATA) + TableSize;

    /* Check if the caller provided a buffer */
    if ((OutBuffer != NULL) && (*InOutBufferSize != 0))
    {
        /* Check if the buffer is large enough */
        if (*InOutBufferSize < ResultSize)
        {
            DPRINT1("Buffer too small. Got %lu, need %lu\n",
                    *InOutBufferSize, ResultSize);
            return STATUS_BUFFER_TOO_SMALL;
        }

        /// FIXME: most of this is fubar
        AllData = OutBuffer;
        AllData->WnodeHeader.BufferSize = ResultSize;
        AllData->WnodeHeader.ProviderId = 0;
        AllData->WnodeHeader.Version = 0;
        AllData->WnodeHeader.Linkage = 0; // last entry
        //AllData->WnodeHeader.CountLost;
        AllData->WnodeHeader.KernelHandle = NULL;
        //AllData->WnodeHeader.TimeStamp;
        AllData->WnodeHeader.Guid = MSSmBios_RawSMBiosTables_GUID;
        //AllData->WnodeHeader.ClientContext;
        AllData->WnodeHeader.Flags = WNODE_FLAG_FIXED_INSTANCE_SIZE;
        AllData->DataBlockOffset = sizeof(WNODE_ALL_DATA);
        AllData->InstanceCount = 1;
        //AllData->OffsetInstanceNameOffsets;
        AllData->FixedInstanceSize = TableSize;

        RtlCopyMemory(AllData + 1, TableData, TableSize);
    }

    /* Set the size */
    *InOutBufferSize = ResultSize;

    /* Free the table buffer */
    if (TableData != NULL)
    {
        ExFreePoolWithTag(TableData, TAG_SMBIOS);
    }

    return STATUS_SUCCESS;
}

