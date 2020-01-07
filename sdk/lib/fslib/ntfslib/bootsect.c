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
FillJumpInstruction(OUT PBOOT_SECTOR BootSector)
{
    BootSector->Jump[0] = 0xEB;  // jmp
    BootSector->Jump[1] = 0x52;  // 82
    BootSector->Jump[2] = 0x90;  // nop
}

static
VOID
FillOemId(OUT PBOOT_SECTOR BootSector)
{
    BootSector->OEMID.QuadPart = 0x202020205346544E;  // NTFS   
}

static
VOID
FillBiosParametersBlock(OUT PBIOS_PARAMETERS_BLOCK  BiosParametersBlock,
                        IN  GET_LENGTH_INFORMATION* LengthInformation,
                        IN  PDISK_GEOMETRY          DiskGeometry)
{
    // See: https://en.wikipedia.org/wiki/BIOS_parameter_block
    
    BiosParametersBlock->BytesPerSector = BPB_BYTES_PER_SECTOR;

    if (LengthInformation->Length.QuadPart < MB_TO_B(512))
    {
        BiosParametersBlock->SectorsPerCluster = 1;
    }
    else if (LengthInformation->Length.QuadPart < MB_TO_B(1024))
    {
        BiosParametersBlock->SectorsPerCluster = 2;
    }
    else if (LengthInformation->Length.QuadPart < MB_TO_B(2048))
    {
        BiosParametersBlock->SectorsPerCluster = 4;
    }
    else
    {
        BiosParametersBlock->SectorsPerCluster = 8;
    }

    // MediaId for hard drives always 0xF8
    BiosParametersBlock->MediaId = (DiskGeometry->MediaType == FixedMedia) ? 0xF8 : 0x00;

    BiosParametersBlock->SectorsPerTrack    = DiskGeometry->SectorsPerTrack;
    BiosParametersBlock->Heads              = BPB_HEADS;
    BiosParametersBlock->HiddenSectorsCount = BPB_WINXP_HIDDEN_SECTORS;
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
FillExBiosParametersBlock(OUT PEXTENDED_BIOS_PARAMETERS_BLOCK ExBiosParametersBlock,
                          IN  GET_LENGTH_INFORMATION*         LengthInformation,
                          IN  PDISK_GEOMETRY                  DiskGeometry)
{
    // See: https://en.wikipedia.org/wiki/BIOS_parameter_block

    ExBiosParametersBlock->Header = EBPB_HEADER;
    ExBiosParametersBlock->SectorCount =
        ((ULONGLONG)DiskGeometry->SectorsPerTrack)   *
        ((ULONGLONG)DiskGeometry->TracksPerCylinder) *
        ((ULONGLONG)DiskGeometry->Cylinders.QuadPart);

    ExBiosParametersBlock->MftLocation = MFT_LOCATION;
    ExBiosParametersBlock->MftMirrLocation = LengthInformation->Length.QuadPart / 2;  // Only for Windows XP

    ExBiosParametersBlock->ClustersPerMftRecord = CLUSTER_PER_MFT_RECORD;
    ExBiosParametersBlock->ClustersPerIndexRecord = 0x01;

    ExBiosParametersBlock->SerialNumber = CalcVolumeSerialNumber();
}

NTSTATUS
WriteBootSector(IN HANDLE                  Handle,
                IN GET_LENGTH_INFORMATION* LengthInformation,
                IN PDISK_GEOMETRY          DiskGeometry)
{
    NTSTATUS        Status;
    IO_STATUS_BLOCK IoStatusBlock;
    PBOOT_SECTOR    BootSector;

    // Allocate memory
    BootSector = RtlAllocateHeap(RtlGetProcessHeap(), 0, sizeof(BOOT_SECTOR));
    if (!BootSector)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Clear memory
    RtlZeroMemory(BootSector, sizeof(BOOT_SECTOR));

    // Fill boot sector structure
    FillJumpInstruction(BootSector);
    FillOemId(BootSector);

    FillBiosParametersBlock(&(BootSector->BPB),    LengthInformation, DiskGeometry);
    FillExBiosParametersBlock(&(BootSector->EBPB), LengthInformation, DiskGeometry);

    BootSector->EndSector = BOOT_SECTOR_END;

    // Write to disk
    Status = NtWriteFile(Handle,
                         NULL,
                         NULL,
                         NULL,
                         &IoStatusBlock,
                         BootSector,
                         sizeof(BOOT_SECTOR),
                         0ULL,
                         NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("BootSector write failed. NtWriteFile() failed (Status %lx)\n", Status);
    }

    // Clear memory
    RtlFreeHeap(RtlGetProcessHeap(), 0, BootSector);

    return Status;
}