/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS NTFS FS library
 * FILE:        lib/fslib/ntfslib/bootsect.c
 * PURPOSE:     NTFS lib
 * PROGRAMMERS: Pierre Schweitzer, Klachkov Valery
 */

/* INCLUDES ******************************************************************/

#include <ntfslib.h>

#define NDEBUG
#include <debug.h>


/* MACROSES ******************************************************************/

#define MB_TO_B(x) (x * 1024)


/* FUNCTIONS *****************************************************************/

static
VOID
WriteJumpInstruction(OUT PBOOT_SECTOR pbs)
{
    pbs->Jump[0] = 0xEB;  // jmp
    pbs->Jump[1] = 0x52;  // 82
    pbs->Jump[2] = 0x90;  // nop
}

static
VOID
WriteBiosParametersBlock(OUT PBIOS_PARAMETERS_BLOCK bpb,
                         IN  GET_LENGTH_INFORMATION* gli,
                         IN  PDISK_GEOMETRY dg)
{
    bpb->BytesPerSector = BPB_BYTES_PER_SECTOR;

    if (gli->Length.QuadPart < MB_TO_B(512))
    {
        bpb->SectorsPerCluster = 1;
    }
    else if (gli->Length.QuadPart < MB_TO_B(1024))
    {
        bpb->SectorsPerCluster = 2;
    }
    else if (gli->Length.QuadPart < MB_TO_B(2048))
    {
        bpb->SectorsPerCluster = 4;
    }
    else
    {
        bpb->SectorsPerCluster = 8;
    }

    bpb->MediaId = (dg->MediaType == FixedMedia) ? 0xF8 : 0x00;
    bpb->SectorsPerTrack = dg->SectorsPerTrack;
    bpb->Heads = BPB_HEADS;
    bpb->HiddenSectorsCount = BPB_WINXP_HIDDEN_SECTORS;
}

static
ULONGLONG
CalcVolumeSerialNumber(VOID)
{
    // FIXME: Ñheck the correctness of the generated serial number

    BYTE  i;
    ULONG r;
    ULONG seed;
    ULONGLONG serial;

    seed = NtGetTickCount();

    for (i = 0; i < 32; i += 2)
    {
        r = RtlRandom(&seed);

        serial |= ((r & 0xff00) >> 8) << (i * 8);
        serial |= ((r & 0xff)) << (i * 8 * 2);
    }

    return serial;
}

static
VOID
WriteExBiosParametersBlock(OUT PEXTENDED_BIOS_PARAMETERS_BLOCK ebpb,
                           IN  GET_LENGTH_INFORMATION* gli,
                           IN  PDISK_GEOMETRY dg)
{
    ebpb->Header = EBPB_HEADER;
    ebpb->SectorCount =
        ((ULONGLONG)dg->SectorsPerTrack) *
        ((ULONGLONG)dg->TracksPerCylinder) *
        ((ULONGLONG)dg->Cylinders.QuadPart);

    ebpb->MftLocation = MFT_LOCATION;
    ebpb->MftMirrLocation = gli->Length.QuadPart / 2;  // Only for Windows XP

    ebpb->ClustersPerMftRecord = CLUSTER_PER_MFT_RECORD;
    ebpb->ClustersPerIndexRecord = 0x01;

    ebpb->SerialNumber = CalcVolumeSerialNumber();
}

NTSTATUS
WriteBootSector(IN HANDLE h,
                IN GET_LENGTH_INFORMATION* gli,
                IN PDISK_GEOMETRY dg)
{
    PBOOT_SECTOR    BootSector;
    NTSTATUS        Status;
    IO_STATUS_BLOCK IoStatusBlock;

    // Allocate memory
    BootSector = RtlAllocateHeap(RtlGetProcessHeap(), 0, sizeof(BOOT_SECTOR));
    if (BootSector == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Clear memory
    RtlZeroMemory(BootSector, sizeof(BOOT_SECTOR));

    // Fill boot sector structure
    WriteJumpInstruction(BootSector);

    BootSector->OEMID.QuadPart = 0x202020205346544E;  // NTFS   

    WriteBiosParametersBlock(&(BootSector->BPB), gli, dg);
    WriteExBiosParametersBlock(&(BootSector->EBPB), gli, dg);

    BootSector->EndSector = BOOT_SECTOR_END;

    // Write to disk
    Status = NtWriteFile(h,
        NULL,
        NULL,
        NULL,
        &IoStatusBlock,
        BootSector,
        sizeof(BOOT_SECTOR),
        0L,
        NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("BootSector write failed. NtWriteFile() failed (Status %lx)\n", Status);
    }

    // Clear memory
    RtlFreeHeap(RtlGetProcessHeap(), 0, BootSector);

    return Status;
}