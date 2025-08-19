/*
 * PROJECT:     FreeLoader UEFI Support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Disk Access Functions
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

/* INCLUDES ******************************************************************/

#include <uefildr.h>

#include <debug.h>
DBG_DEFAULT_CHANNEL(WARNING);

/* UEFI Protocol GUIDs */
static EFI_GUID gEfiBlockIoProtocolGuid = {
    0x964e5b21, 0x6459, 0x11d2, {0x8e, 0x39, 0x0, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};

#define TAG_HW_RESOURCE_LIST    'lRwH'
#define TAG_HW_DISK_CONTEXT     'cDwH'
#define FIRST_BIOS_DISK 0x80
#define FIRST_PARTITION 1

typedef struct tagDISKCONTEXT
{
    UCHAR DriveNumber;
    ULONG SectorSize;
    ULONGLONG SectorOffset;
    ULONGLONG SectorCount;
    ULONGLONG SectorNumber;
} DISKCONTEXT;

typedef struct _INTERNAL_UEFI_DISK
{
    UCHAR ArcDriveNumber;
    UCHAR NumOfPartitions;
    UCHAR UefiRootNumber;
    BOOLEAN IsThisTheBootDrive;
} INTERNAL_UEFI_DISK, *PINTERNAL_UEFI_DISK;

/* GLOBALS *******************************************************************/

extern EFI_SYSTEM_TABLE* GlobalSystemTable;
extern EFI_HANDLE GlobalImageHandle;
extern EFI_HANDLE PublicBootHandle; /* Freeldr itself */

/* Made to match BIOS */
PVOID DiskReadBuffer;
UCHAR PcBiosDiskCount;

UCHAR FrldrBootDrive;
ULONG FrldrBootPartition;
SIZE_T DiskReadBufferSize;
PVOID Buffer;

static const CHAR Hex[] = "0123456789abcdef";
static CHAR PcDiskIdentifier[32][20];

/* UEFI-specific */
static ULONG UefiBootRootIdentifier;
static ULONG OffsetToBoot;
static ULONG PublicBootArcDisk;
static INTERNAL_UEFI_DISK* InternalUefiDisk = NULL;
static EFI_GUID BlockIoGuid = {
    0x964e5b21, 0x6459, 0x11d2, {0x8e, 0x39, 0x0, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};
static EFI_GUID LoadedImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_BLOCK_IO* bio;
static EFI_HANDLE* BlockIoHandles = NULL;
static UINTN BlockIoHandleCount = 0;
static EFI_HANDLE BootDeviceHandle = NULL;

/* FUNCTIONS *****************************************************************/

PCHAR
GetHarddiskIdentifier(UCHAR DriveNumber)
{
    TRACE("GetHarddiskIdentifier: DriveNumber: %d\n", DriveNumber);
    return PcDiskIdentifier[DriveNumber - FIRST_BIOS_DISK];
}

static LONG lReportError = 0; // >= 0: display errors; < 0: hide errors.

LONG
DiskReportError(BOOLEAN bShowError)
{
    /* Set the reference count */
    if (bShowError) ++lReportError;
    else            --lReportError;
    return lReportError;
}

static
BOOLEAN
UefiDetectBootDevice(VOID)
{
    EFI_STATUS Status;
    EFI_LOADED_IMAGE_PROTOCOL* LoadedImage;

    TRACE("UefiDetectBootDevice: Detecting boot device\n");

    /* Get the loaded image protocol to find our device handle */
    Status = GlobalSystemTable->BootServices->HandleProtocol(
        GlobalImageHandle,
        &LoadedImageGuid,
        (VOID**)&LoadedImage);
    
    if (EFI_ERROR(Status))
    {
        ERR("Failed to get LoadedImage protocol: 0x%lx\n", Status);
        return FALSE;
    }

    BootDeviceHandle = LoadedImage->DeviceHandle;
    TRACE("Boot device handle: %p\n", BootDeviceHandle);
    
    return TRUE;
}

static
BOOLEAN
UefiFindBootDeviceInEnumeration(VOID)
{
    ULONG i;
    
    TRACE("UefiFindBootDeviceInEnumeration: Looking for boot device in block device list\n");
    
    if (!BootDeviceHandle)
    {
        ERR("Boot device handle not set\n");
        return FALSE;
    }
    
    /* Find this device in our block device list */
    for (i = 0; i < BlockIoHandleCount; i++)
    {
        if (BlockIoHandles[i] == BootDeviceHandle)
        {
            UefiBootRootIdentifier = i;
            TRACE("Found boot device at block device index %lu\n", i);
            return TRUE;
        }
    }

    ERR("Boot device not found in block device list\n");
    return FALSE;
}

static
ARC_STATUS
UefiDiskClose(ULONG FileId)
{
    DISKCONTEXT* Context = FsGetDeviceSpecific(FileId);
    FrLdrTempFree(Context, TAG_HW_DISK_CONTEXT);
    return ESUCCESS;
}

static
ARC_STATUS
UefiDiskGetFileInformation(ULONG FileId, FILEINFORMATION *Information)
{
    DISKCONTEXT* Context = FsGetDeviceSpecific(FileId);
    RtlZeroMemory(Information, sizeof(*Information));

    /*
     * The ARC specification mentions that for partitions, StartingAddress and
     * EndingAddress are the start and end positions of the partition in terms
     * of byte offsets from the start of the disk.
     * CurrentAddress is the current offset into (i.e. relative to) the partition.
     */
    Information->StartingAddress.QuadPart = Context->SectorOffset * Context->SectorSize;
    Information->EndingAddress.QuadPart   = (Context->SectorOffset + Context->SectorCount) * Context->SectorSize;
    Information->CurrentAddress.QuadPart  = Context->SectorNumber * Context->SectorSize;

    return ESUCCESS;
}

static
ARC_STATUS
UefiDiskOpen(CHAR *Path, OPENMODE OpenMode, ULONG *FileId)
{
    DISKCONTEXT* Context;
    UCHAR DriveNumber;
    ULONG DrivePartition, SectorSize;
    ULONGLONG SectorOffset = 0;
    ULONGLONG SectorCount = 0;
    ULONG UefiDriveNumber = 0;
    PARTITION_TABLE_ENTRY PartitionTableEntry;
    EFI_STATUS Status;

    TRACE("UefiDiskOpen: File ID: %d, Path: %s\n", FileId, Path);

    if (DiskReadBufferSize == 0)
    {
        ERR("DiskOpen(): DiskReadBufferSize is 0, something is wrong.\n");
        ASSERT(FALSE);
        return ENOMEM;
    }

    if (!DissectArcPath(Path, NULL, &DriveNumber, &DrivePartition))
        return EINVAL;

    TRACE("Opening disk: DriveNumber: %d, DrivePartition: %d\n", DriveNumber, DrivePartition);
    UefiDriveNumber = DriveNumber - FIRST_BIOS_DISK;
    
    if (UefiDriveNumber >= PcBiosDiskCount)
    {
        ERR("Invalid drive number: %d (max: %d)\n", UefiDriveNumber, PcBiosDiskCount - 1);
        return EINVAL;
    }
    
    Status = GlobalSystemTable->BootServices->HandleProtocol(
        BlockIoHandles[InternalUefiDisk[UefiDriveNumber].UefiRootNumber], 
        &BlockIoGuid, 
        (VOID**)&bio);
    
    if (EFI_ERROR(Status))
    {
        ERR("Failed to get Block IO protocol: 0x%lx\n", Status);
        return EIO;
    }
    
    SectorSize = bio->Media->BlockSize;

    if (DrivePartition != 0xff && DrivePartition != 0)
    {
        if (!DiskGetPartitionEntry(DriveNumber, DrivePartition, &PartitionTableEntry))
            return EINVAL;

        SectorOffset = PartitionTableEntry.SectorCountBeforePartition;
        SectorCount = PartitionTableEntry.PartitionSectorCount;
    }
    else
    {
        GEOMETRY Geometry;
        if (!MachDiskGetDriveGeometry(DriveNumber, &Geometry))
            return EINVAL;

        if (SectorSize != Geometry.BytesPerSector)
        {
            ERR("SectorSize (%lu) != Geometry.BytesPerSector (%lu), expect problems!\n",
                SectorSize, Geometry.BytesPerSector);
        }

        SectorOffset = 0;
        SectorCount = Geometry.Sectors;
    }

    Context = FrLdrTempAlloc(sizeof(DISKCONTEXT), TAG_HW_DISK_CONTEXT);
    if (!Context)
        return ENOMEM;

    Context->DriveNumber = DriveNumber;
    Context->SectorSize = SectorSize;
    Context->SectorOffset = SectorOffset;
    Context->SectorCount = SectorCount;
    Context->SectorNumber = 0;
    FsSetDeviceSpecific(*FileId, Context);
    return ESUCCESS;
}

static
ARC_STATUS
UefiDiskRead(ULONG FileId, VOID *Buffer, ULONG N, ULONG *Count)
{
    DISKCONTEXT* Context = FsGetDeviceSpecific(FileId);
    UCHAR* Ptr = (UCHAR*)Buffer;
    ULONG Length, TotalSectors, MaxSectors, ReadSectors;
    ULONGLONG SectorOffset;
    BOOLEAN ret;

    ASSERT(DiskReadBufferSize > 0);

    TotalSectors = (N + Context->SectorSize - 1) / Context->SectorSize;
    MaxSectors   = DiskReadBufferSize / Context->SectorSize;
    SectorOffset = Context->SectorOffset + Context->SectorNumber;

    // If MaxSectors is 0, this will lead to infinite loop.
    // In release builds assertions are disabled, however we also have sanity checks in DiskOpen()
    ASSERT(MaxSectors > 0);

    ret = TRUE;

    while (TotalSectors)
    {
        ReadSectors = min(TotalSectors, MaxSectors);

        ret = MachDiskReadLogicalSectors(Context->DriveNumber,
                                         SectorOffset,
                                         ReadSectors,
                                         DiskReadBuffer);
        if (!ret)
            break;

        Length = ReadSectors * Context->SectorSize;
        Length = min(Length, N);

        RtlCopyMemory(Ptr, DiskReadBuffer, Length);

        Ptr += Length;
        N -= Length;
        SectorOffset += ReadSectors;
        TotalSectors -= ReadSectors;
    }

    *Count = (ULONG)((ULONG_PTR)Ptr - (ULONG_PTR)Buffer);
    Context->SectorNumber = SectorOffset - Context->SectorOffset;

    return (ret ? ESUCCESS : EIO);
}

static
ARC_STATUS
UefiDiskSeek(ULONG FileId, LARGE_INTEGER *Position, SEEKMODE SeekMode)
{
    DISKCONTEXT* Context = FsGetDeviceSpecific(FileId);
    LARGE_INTEGER NewPosition = *Position;

    switch (SeekMode)
    {
        case SeekAbsolute:
            break;
        case SeekRelative:
            NewPosition.QuadPart += (Context->SectorNumber * Context->SectorSize);
            break;
        default:
            ASSERT(FALSE);
            return EINVAL;
    }

    if (NewPosition.QuadPart & (Context->SectorSize - 1))
        return EINVAL;

    /* Convert in number of sectors */
    NewPosition.QuadPart /= Context->SectorSize;

    /* HACK: CDROMs may have a SectorCount of 0 */
    if (Context->SectorCount != 0 && NewPosition.QuadPart >= Context->SectorCount)
        return EINVAL;

    Context->SectorNumber = NewPosition.QuadPart;
    return ESUCCESS;
}

static const DEVVTBL UefiDiskVtbl =
{
    UefiDiskClose,
    UefiDiskGetFileInformation,
    UefiDiskOpen,
    UefiDiskRead,
    UefiDiskSeek,
};

static
VOID
GetHarddiskInformation(UCHAR DriveNumber)
{
    PMASTER_BOOT_RECORD Mbr;
    PULONG Buffer;
    ULONG i;
    ULONG Checksum;
    ULONG Signature;
    BOOLEAN ValidPartitionTable;
    CHAR ArcName[MAX_PATH];
    PARTITION_TABLE_ENTRY PartitionTableEntry;
    PCHAR Identifier = PcDiskIdentifier[DriveNumber - FIRST_BIOS_DISK];

    /* Detect disk partition type */
    DiskDetectPartitionType(DriveNumber);

    /* Read the MBR */
    if (!MachDiskReadLogicalSectors(DriveNumber, 0ULL, 1, DiskReadBuffer))
    {
        ERR("Reading MBR failed\n");
        /* We failed, use a default identifier */
        sprintf(Identifier, "BIOSDISK%d", DriveNumber - FIRST_BIOS_DISK);
        return;
    }

    Buffer = (ULONG*)DiskReadBuffer;
    Mbr = (PMASTER_BOOT_RECORD)DiskReadBuffer;

    Signature = Mbr->Signature;
    TRACE("Signature: %x\n", Signature);

    /* Calculate the MBR checksum */
    Checksum = 0;
    for (i = 0; i < 512 / sizeof(ULONG); i++)
    {
        Checksum += Buffer[i];
    }
    Checksum = ~Checksum + 1;
    TRACE("Checksum: %x\n", Checksum);

    ValidPartitionTable = (Mbr->MasterBootRecordMagic == 0xAA55);

    /* Fill out the ARC disk block */
    sprintf(ArcName, "multi(0)disk(0)rdisk(%u)", DriveNumber - FIRST_BIOS_DISK);
    AddReactOSArcDiskInfo(ArcName, Signature, Checksum, ValidPartitionTable);

    sprintf(ArcName, "multi(0)disk(0)rdisk(%u)partition(0)", DriveNumber - FIRST_BIOS_DISK);
    FsRegisterDevice(ArcName, &UefiDiskVtbl);

    /* Add partitions */
    i = FIRST_PARTITION;
    DiskReportError(FALSE);
    while (DiskGetPartitionEntry(DriveNumber, i, &PartitionTableEntry))
    {
        if (PartitionTableEntry.SystemIndicator != PARTITION_ENTRY_UNUSED)
        {
            sprintf(ArcName, "multi(0)disk(0)rdisk(%u)partition(%lu)", DriveNumber - FIRST_BIOS_DISK, i);
            FsRegisterDevice(ArcName, &UefiDiskVtbl);
        }
        i++;
    }
    DiskReportError(TRUE);

    InternalUefiDisk[DriveNumber - FIRST_BIOS_DISK].NumOfPartitions = i;
    /* Convert checksum and signature to identifier string */
    Identifier[0] = Hex[(Checksum >> 28) & 0x0F];
    Identifier[1] = Hex[(Checksum >> 24) & 0x0F];
    Identifier[2] = Hex[(Checksum >> 20) & 0x0F];
    Identifier[3] = Hex[(Checksum >> 16) & 0x0F];
    Identifier[4] = Hex[(Checksum >> 12) & 0x0F];
    Identifier[5] = Hex[(Checksum >> 8) & 0x0F];
    Identifier[6] = Hex[(Checksum >> 4) & 0x0F];
    Identifier[7] = Hex[Checksum & 0x0F];
    Identifier[8] = '-';
    Identifier[9] = Hex[(Signature >> 28) & 0x0F];
    Identifier[10] = Hex[(Signature >> 24) & 0x0F];
    Identifier[11] = Hex[(Signature >> 20) & 0x0F];
    Identifier[12] = Hex[(Signature >> 16) & 0x0F];
    Identifier[13] = Hex[(Signature >> 12) & 0x0F];
    Identifier[14] = Hex[(Signature >> 8) & 0x0F];
    Identifier[15] = Hex[(Signature >> 4) & 0x0F];
    Identifier[16] = Hex[Signature & 0x0F];
    Identifier[17] = '-';
    Identifier[18] = (ValidPartitionTable ? 'A' : 'X');
    Identifier[19] = 0;
    TRACE("Identifier: %s\n", Identifier);
}

static
VOID
UefiEnumerateBlockDevices(VOID)
{
    EFI_STATUS Status;
    UINTN HandleBufferSize = 0;
    ULONG BlockDeviceIndex = 0;
    ULONG i;

    TRACE("UefiEnumerateBlockDevices: Enumerating all block devices\n");

    /* Reset counters */
    PcBiosDiskCount = 0;
    BlockIoHandleCount = 0;

    /* Get the size needed for all Block IO handles */
    Status = GlobalSystemTable->BootServices->LocateHandle(
        ByProtocol,
        &BlockIoGuid,
        NULL,
        &HandleBufferSize,
        NULL);
    
    /* Allocate buffer for handles */
    BlockIoHandles = MmAllocateMemoryWithType(HandleBufferSize, LoaderFirmwareTemporary);
    if (!BlockIoHandles)
    {
        ERR("Failed to allocate memory for Block IO handles\n");
        return;
    }

    /* Get all Block IO handles */
    Status = GlobalSystemTable->BootServices->LocateHandle(
        ByProtocol,
        &BlockIoGuid,
        NULL,
        &HandleBufferSize,
        BlockIoHandles);
    
    if (EFI_ERROR(Status))
    {
        ERR("Failed to locate Block IO handles: 0x%lx\n", Status);
        return;
    }

    BlockIoHandleCount = HandleBufferSize / sizeof(EFI_HANDLE);
    TRACE("Found %lu Block IO handles\n", BlockIoHandleCount);

    /* Allocate array for internal disk tracking */
    InternalUefiDisk = MmAllocateMemoryWithType(
        sizeof(INTERNAL_UEFI_DISK) * BlockIoHandleCount,
        LoaderFirmwareTemporary);
    
    if (!InternalUefiDisk)
    {
        ERR("Failed to allocate memory for internal disk array\n");
        return;
    }

    RtlZeroMemory(InternalUefiDisk, sizeof(INTERNAL_UEFI_DISK) * BlockIoHandleCount);

    /* Enumerate each block device */
    for (i = 0; i < BlockIoHandleCount; i++)
    {
        Status = GlobalSystemTable->BootServices->HandleProtocol(
            BlockIoHandles[i],
            &BlockIoGuid,
            (VOID**)&bio);
        
        if (EFI_ERROR(Status))
        {
            TRACE("Failed to get Block IO protocol for handle %lu: 0x%lx\n", i, Status);
            continue;
        }

        /* Skip invalid devices */
        if (bio->Media->BlockSize == 0 || bio->Media->BlockSize > 4096)
        {
            TRACE("Skipping device %lu with invalid block size %lu\n", i, bio->Media->BlockSize);
            continue;
        }

        /* Only process physical disks (not logical partitions) for ARC enumeration */
        if (bio->Media->LogicalPartition == FALSE)
        {
            TRACE("Found physical disk at index %lu\n", i);
            
            /* Set up internal disk structure BEFORE incrementing counters */
            InternalUefiDisk[BlockDeviceIndex].ArcDriveNumber = BlockDeviceIndex;
            InternalUefiDisk[BlockDeviceIndex].UefiRootNumber = i;
            InternalUefiDisk[BlockDeviceIndex].NumOfPartitions = 0;
            InternalUefiDisk[BlockDeviceIndex].IsThisTheBootDrive = FALSE;
            
            /* Increment counters BEFORE processing the disk so validation functions work */
            PcBiosDiskCount++;
            
            /* Process this physical disk */
            GetHarddiskInformation(BlockDeviceIndex + FIRST_BIOS_DISK);
            
            /* Increment BlockDeviceIndex for next disk */
            BlockDeviceIndex++;
        }
    }

    TRACE("Enumerated %lu physical disks\n", PcBiosDiskCount);
}

static
BOOLEAN
UefiSetBootpath(VOID)
{
    EFI_STATUS Status;
    EFI_BLOCK_IO_PROTOCOL* BootDeviceBio;
    
    TRACE("UefiSetBootpath: Setting up boot path\n");

    if (!BootDeviceHandle)
    {
        ERR("Boot device handle not set\n");
        return FALSE;
    }

    /* Get Block IO protocol for boot device */
    Status = GlobalSystemTable->BootServices->HandleProtocol(
        BootDeviceHandle,
        &BlockIoGuid,
        (VOID**)&BootDeviceBio);
    
    if (EFI_ERROR(Status))
    {
        ERR("Failed to get Block IO for boot device: 0x%lx\n", Status);
        return FALSE;
    }

    FrldrBootDrive = (FIRST_BIOS_DISK + PublicBootArcDisk);

    /* Check if this is a CD-ROM/DVD (removable media with 2048-byte sectors) */
    if (BootDeviceBio->Media->RemovableMedia && BootDeviceBio->Media->BlockSize == 2048)
    {
        /* Boot Partition 0xFF is the magic value that indicates booting from CD-ROM (see isoboot.S) */
        FrldrBootPartition = 0xFF;
        RtlStringCbPrintfA(FrLdrBootPath, sizeof(FrLdrBootPath),
                           "multi(0)disk(0)cdrom(%u)", PublicBootArcDisk);
        TRACE("Boot path (CD-ROM): %s\n", FrLdrBootPath);
    }
    else
    {
        /* This is a hard disk or similar block device */
        if (BootDeviceBio->Media->LogicalPartition)
        {
            /* We're booting from a partition - need to determine which one */
            /* For now, assume partition 1 - this could be enhanced to parse GPT/MBR */
            FrldrBootPartition = 1;
            RtlStringCbPrintfA(FrLdrBootPath, sizeof(FrLdrBootPath),
                               "multi(0)disk(0)rdisk(%u)partition(%lu)",
                               PublicBootArcDisk, FrldrBootPartition);
        }
        else
        {
            /* Booting from raw disk */
            FrldrBootPartition = 0;
            RtlStringCbPrintfA(FrLdrBootPath, sizeof(FrLdrBootPath),
                               "multi(0)disk(0)rdisk(%u)partition(0)",
                               PublicBootArcDisk);
        }
        TRACE("Boot path (HDD): %s\n", FrLdrBootPath);
    }

    return TRUE;
}

static
BOOLEAN
UefiIdentifyBootDrive(VOID)
{
    EFI_STATUS Status;
    EFI_BLOCK_IO_PROTOCOL* BootDeviceBio;
    ULONG i, j;
    BOOLEAN FoundBootDrive = FALSE;

    TRACE("UefiIdentifyBootDrive: Identifying boot drive\n");

    if (!BootDeviceHandle)
    {
        ERR("Boot device handle not set\n");
        return FALSE;
    }

    /* Get Block IO protocol for boot device */
    Status = GlobalSystemTable->BootServices->HandleProtocol(
        BootDeviceHandle,
        &BlockIoGuid,
        (VOID**)&BootDeviceBio);
    
    if (EFI_ERROR(Status))
    {
        ERR("Failed to get Block IO for boot device: 0x%lx\n", Status);
        return FALSE;
    }

    /* Check if boot device is a logical partition */
    if (BootDeviceBio->Media->LogicalPartition)
    {
        TRACE("Boot device is a logical partition\n");
        
        /* Find the parent physical disk by matching MediaId and other characteristics */
        for (i = 0; i < BlockIoHandleCount; i++)
        {
            Status = GlobalSystemTable->BootServices->HandleProtocol(
                BlockIoHandles[i],
                &BlockIoGuid,
                (VOID**)&bio);
            
            if (EFI_ERROR(Status))
                continue;

            /* Skip logical partitions when looking for the parent disk */
            if (bio->Media->LogicalPartition)
                continue;

            /* Try to match this physical disk with our boot partition's parent */
            /* This is a heuristic - UEFI doesn't provide direct parent/child relationships */
            if (bio->Media->MediaId == BootDeviceBio->Media->MediaId ||
                (bio->Media->BlockSize == BootDeviceBio->Media->BlockSize &&
                 bio->Media->RemovableMedia == BootDeviceBio->Media->RemovableMedia))
            {
                /* Find this physical disk in our ARC enumeration */
                for (j = 0; j < PcBiosDiskCount; j++)
                {
                    if (InternalUefiDisk[j].UefiRootNumber == i)
                    {
                        InternalUefiDisk[j].IsThisTheBootDrive = TRUE;
                        PublicBootArcDisk = j;
                        FoundBootDrive = TRUE;
                        TRACE("Boot drive is ARC disk %lu (physical disk at UEFI index %lu)\n", j, i);
                        break;
                    }
                }
                
                if (FoundBootDrive)
                    break;
            }
        }
    }
    else
    {
        TRACE("Boot device is a physical disk\n");
        
        /* Boot device is the physical disk itself */
        for (i = 0; i < PcBiosDiskCount; i++)
        {
            if (BlockIoHandles[InternalUefiDisk[i].UefiRootNumber] == BootDeviceHandle)
            {
                InternalUefiDisk[i].IsThisTheBootDrive = TRUE;
                PublicBootArcDisk = i;
                FoundBootDrive = TRUE;
                TRACE("Boot drive is ARC disk %lu\n", i);
                break;
            }
        }
    }

    if (!FoundBootDrive)
    {
        ERR("Failed to identify boot drive in ARC enumeration\n");
        return FALSE;
    }

    return TRUE;
}

UCHAR
UefiGetFloppyCount(VOID)
{
    /* No floppy for you for now... */
    return 0;
}

BOOLEAN
UefiDiskReadLogicalSectors(
    IN UCHAR DriveNumber,
    IN ULONGLONG SectorNumber,
    IN ULONG SectorCount,
    OUT PVOID Buffer)
{
    EFI_STATUS Status;
    ULONG UefiDriveNumber;

    if (DriveNumber < FIRST_BIOS_DISK)
    {
        ERR("Invalid drive number: %d\n", DriveNumber);
        return FALSE;
    }

    UefiDriveNumber = DriveNumber - FIRST_BIOS_DISK;
    
    if (UefiDriveNumber >= PcBiosDiskCount || !InternalUefiDisk)
    {
        ERR("Drive number %d out of range (max: %d)\n", UefiDriveNumber, PcBiosDiskCount - 1);
        return FALSE;
    }


    Status = GlobalSystemTable->BootServices->HandleProtocol(
        BlockIoHandles[InternalUefiDisk[UefiDriveNumber].UefiRootNumber], 
        &BlockIoGuid, 
        (VOID**)&bio);
    
    if (EFI_ERROR(Status))
    {
        ERR("Failed to get Block IO protocol: 0x%lx\n", Status);
        return FALSE;
    }

    /* Perform the actual read */
    Status = bio->ReadBlocks(bio, bio->Media->MediaId, SectorNumber, 
                            SectorCount * bio->Media->BlockSize, Buffer);
    
    if (EFI_ERROR(Status))
    {
        ERR("ReadBlocks failed: 0x%lx\n", Status);
        return FALSE;
    }

    return TRUE;
}

BOOLEAN
UefiDiskGetDriveGeometry(UCHAR DriveNumber, PGEOMETRY Geometry)
{
    EFI_STATUS Status;
    ULONG UefiDriveNumber;

    if (DriveNumber < FIRST_BIOS_DISK)
    {
        ERR("Invalid drive number: %d\n", DriveNumber);
        return FALSE;
    }

    UefiDriveNumber = DriveNumber - FIRST_BIOS_DISK;
    
    if (UefiDriveNumber >= PcBiosDiskCount || !InternalUefiDisk)
    {
        ERR("Drive number %d out of range (max: %d)\n", UefiDriveNumber, PcBiosDiskCount - 1);
        return FALSE;
    }

    Status = GlobalSystemTable->BootServices->HandleProtocol(
        BlockIoHandles[InternalUefiDisk[UefiDriveNumber].UefiRootNumber], 
        &BlockIoGuid, 
        (VOID**)&bio);
    
    if (EFI_ERROR(Status))
    {
        ERR("Failed to get Block IO protocol: 0x%lx\n", Status);
        return FALSE;
    }

    /* Fill geometry structure - UEFI Block IO doesn't use CHS addressing */
    Geometry->Cylinders = 1; // Not relevant for UEFI Block IO protocol
    Geometry->Heads = 1;     // Not relevant for UEFI Block IO protocol
  //  Geometry->SectorsPerTrack = (ULONG)(bio->Media->LastBlock + 1);
    Geometry->BytesPerSector = bio->Media->BlockSize;
    Geometry->Sectors = (ULONGLONG)(bio->Media->LastBlock + 1);

    TRACE("Drive %d geometry: %llu sectors, %lu bytes/sector\n", 
          DriveNumber, Geometry->Sectors, Geometry->BytesPerSector);

    return TRUE;
}

ULONG
UefiDiskGetCacheableBlockCount(UCHAR DriveNumber)
{
    EFI_STATUS Status;
    ULONG UefiDriveNumber;

    if (DriveNumber < FIRST_BIOS_DISK)
    {
        ERR("Invalid drive number: %d\n", DriveNumber);
        return 0;
    }

    UefiDriveNumber = DriveNumber - FIRST_BIOS_DISK;
    
    if (UefiDriveNumber >= PcBiosDiskCount || !InternalUefiDisk)
    {
        ERR("Drive number %d out of range (max: %d)\n", UefiDriveNumber, PcBiosDiskCount - 1);
        return 0;
    }

    TRACE("UefiDiskGetCacheableBlockCount: ARC Drive %d -> UEFI Handle Index %d\n", 
          UefiDriveNumber, InternalUefiDisk[UefiDriveNumber].UefiRootNumber);

    Status = GlobalSystemTable->BootServices->HandleProtocol(
        BlockIoHandles[InternalUefiDisk[UefiDriveNumber].UefiRootNumber], 
        &BlockIoGuid, 
        (VOID**)&bio);
    
    if (EFI_ERROR(Status))
    {
        ERR("Failed to get Block IO protocol: 0x%lx\n", Status);
        return 0;
    }

    return (ULONG)(bio->Media->LastBlock + 1);
}

BOOLEAN
UefiInitializeBootDevices(VOID)
{
    EFI_STATUS Status;
    EFI_BLOCK_IO_PROTOCOL* BootDeviceBio;

    TRACE("UefiInitializeBootDevices: Starting UEFI boot device initialization\n");

    /* Initialize disk read buffer */
    DiskReadBufferSize = EFI_PAGE_SIZE;
    DiskReadBuffer = MmAllocateMemoryWithType(DiskReadBufferSize, LoaderFirmwareTemporary);
    if (!DiskReadBuffer)
    {
        ERR("Failed to allocate disk read buffer\n");
        return FALSE;
    }

    /* Step 1: Detect boot device (where freeldr.ini resides) */
    if (!UefiDetectBootDevice())
    {
        ERR("Failed to detect boot device\n");
        return FALSE;
    }

    /* Step 2: Enumerate all block devices and create ARC mappings */
    UefiEnumerateBlockDevices();

    /* Step 2.5: Find boot device in our enumeration */
    if (!UefiFindBootDeviceInEnumeration())
    {
        ERR("Failed to find boot device in enumeration\n");
        return FALSE;
    }

    /* Step 3: Identify which ARC disk corresponds to our boot device */
    if (!UefiIdentifyBootDrive())
    {
        ERR("Failed to identify boot drive\n");
        return FALSE;
    }

    /* Step 4: Set up boot path */
    if (!UefiSetBootpath())
    {
        ERR("Failed to set boot path\n");
        return FALSE;
    }

    /* Step 5: Handle special case for CD-ROM boot devices */
    Status = GlobalSystemTable->BootServices->HandleProtocol(
        BootDeviceHandle,
        &BlockIoGuid,
        (VOID**)&BootDeviceBio);
    
    if (!EFI_ERROR(Status) && 
        BootDeviceBio->Media->RemovableMedia && 
        BootDeviceBio->Media->BlockSize == 2048)
    {
        PMASTER_BOOT_RECORD Mbr;
        PULONG Buffer;
        ULONG Checksum = 0;
        ULONG Signature;
        ULONG i;

        TRACE("Boot device is CD-ROM, reading volume descriptor\n");

        /* Read the CD-ROM volume descriptor (sector 16) */
        if (MachDiskReadLogicalSectors(FrldrBootDrive, 16ULL, 1, DiskReadBuffer))
        {
            Buffer = (ULONG*)DiskReadBuffer;
            Mbr = (PMASTER_BOOT_RECORD)DiskReadBuffer;

            Signature = Mbr->Signature;
            TRACE("CD-ROM Signature: %x\n", Signature);

            /* Calculate the checksum */
            for (i = 0; i < 2048 / sizeof(ULONG); i++)
            {
                Checksum += Buffer[i];
            }
            Checksum = ~Checksum + 1;
            TRACE("CD-ROM Checksum: %x\n", Checksum);

            /* Register the CD-ROM device with ARC system */
            AddReactOSArcDiskInfo(FrLdrBootPath, Signature, Checksum, TRUE);
            FsRegisterDevice(FrLdrBootPath, &UefiDiskVtbl);
            
            TRACE("CD-ROM boot device registered: %s\n", FrLdrBootPath);
        }
        else
        {
            ERR("Failed to read CD-ROM volume descriptor\n");
        }
    }

    TRACE("UEFI boot device initialization completed successfully\n");
    TRACE("Boot drive: 0x%02X, Boot path: %s\n", (int)FrldrBootDrive, FrLdrBootPath);
    
    return TRUE;
}
