
#include <uefildr.h>


#include <debug.h>
DBG_DEFAULT_CHANNEL(WARNING);

extern EFI_SYSTEM_TABLE * GlobalSystemTable;
extern EFI_HANDLE GlobalImageHandle;
extern EFI_HANDLE PublicBootHandle;

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

UCHAR FrldrBootDrive;
ULONG FrldrBootPartition;
SIZE_T DiskReadBufferSize;
void* DiskReadBuffer;
PVOID Buffer;
UINT64* LoaderFileSize;

ULONG UefiBootRootIdentifier;
 ULONG OffsetToBoot;
static const CHAR Hex[] = "0123456789abcdef";
static CHAR PcDiskIdentifier[32][20];
UCHAR PcBiosDiskCount = 0; //lmao

EFI_GUID bioGuid = BLOCK_IO_PROTOCOL;
EFI_BLOCK_IO *bio;
EFI_HANDLE *handles = NULL;
UINTN handle_size = 0;

//BLOCKINFO bi;
static ARC_STATUS
UefiDiskClose(ULONG FileId)
{
    DISKCONTEXT* Context = FsGetDeviceSpecific(FileId);
    FrLdrTempFree(Context, TAG_HW_DISK_CONTEXT);
    return ESUCCESS;
}

static ARC_STATUS
UefiDiskGetFileInformation(ULONG FileId, FILEINFORMATION *Information)
{
    TRACE("UefiDiskGetFileInformation: File ID: %d\n", FileId);
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

static ARC_STATUS
UefiDiskOpen(CHAR *Path, OPENMODE OpenMode, ULONG *FileId)
{
    TRACE("UefiDiskOpen: File ID: %d\nPath:%s\n", FileId, Path);
    DISKCONTEXT* Context;
    UCHAR DriveNumber;
    ULONG DrivePartition, SectorSize;
    ULONGLONG SectorOffset = 0;
    ULONGLONG SectorCount = 0;
    ULONG UefiDriveNumber = 0;
    PARTITION_TABLE_ENTRY PartitionTableEntry;

    if (DiskReadBufferSize == 0)
    {
        ERR("DiskOpen(): DiskReadBufferSize is 0, something is wrong.\n");
        ASSERT(FALSE);
        return ENOMEM;
    }

    if (!DissectArcPath(Path, NULL, &DriveNumber, &DrivePartition))
        return EINVAL;
    TRACE("Opening disk:\n Drive Number: %d\n DrivePartition: %d", DriveNumber, DrivePartition);
    TRACE("UefiDiskGetDriveGeometry: DriveNumber: %d\n", DriveNumber);
    UefiDriveNumber = DriveNumber - FIRST_BIOS_DISK;
    GlobalSystemTable->BootServices->HandleProtocol(handles[UefiDriveNumber], &bioGuid, (void **) &bio);
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
        SectorCount = (ULONGLONG)Geometry.Cylinders * Geometry.Heads * Geometry.Sectors;
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

static ARC_STATUS
UefiDiskRead(ULONG FileId, VOID *Buffer, ULONG N, ULONG *Count)
{
    TRACE("UefiDiskRead: File ID: %d\n", FileId);
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
        ReadSectors = TotalSectors;
        if (ReadSectors > MaxSectors)
            ReadSectors = MaxSectors;

        ret = MachDiskReadLogicalSectors(Context->DriveNumber,
                                         SectorOffset,
                                         ReadSectors,
                                         DiskReadBuffer);
        if (!ret)
            break;

        Length = ReadSectors * Context->SectorSize;
        if (Length > N)
            Length = N;

        RtlCopyMemory(Ptr, DiskReadBuffer, Length);

        Ptr += Length;
        N -= Length;
        SectorOffset += ReadSectors;
        TotalSectors -= ReadSectors;
    }

    *Count = (ULONG)((ULONG_PTR)Ptr - (ULONG_PTR)Buffer);
    Context->SectorNumber = SectorOffset - Context->SectorOffset;

    return (!ret) ? EIO : ESUCCESS;
}

static ARC_STATUS
UefiDiskSeek(ULONG FileId, LARGE_INTEGER *Position, SEEKMODE SeekMode)
{
    TRACE("UefiDiskSeek: File ID: %d\n", FileId);
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
//TODO
BOOLEAN
UefiGetBootPartitionEntry(
    IN UCHAR DriveNumber,
    OUT PPARTITION_TABLE_ENTRY PartitionTableEntry,
    OUT PULONG BootPartition)
{
    TRACE("UefiGetBootPartitionEntry: DriveNumber: %d\n", DriveNumber);
    *BootPartition = (OffsetToBoot - UefiBootRootIdentifier);
    TRACE("Boot Partition %d", OffsetToBoot);
    return TRUE;
}

PCHAR
GetHarddiskIdentifier(UCHAR DriveNumber)
{
    TRACE("GetHarddiskIdentifier: DriveNumber: %d\n", DriveNumber);
    return PcDiskIdentifier[DriveNumber - FIRST_BIOS_DISK];
}

static VOID
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
    PCHAR Identifier = PcDiskIdentifier[(DriveNumber - FIRST_BIOS_DISK)];

    /* Detect disk partition type */
    DiskDetectPartitionType(DriveNumber);

    /* Read the MBR */
    if (!MachDiskReadLogicalSectors(DriveNumber, 0ULL, 1, DiskReadBuffer))
    {
        ERR("Reading MBR failed\n");
        /* We failed, use a default identifier */
        sprintf(Identifier, "BIOSDISK%d", DriveNumber - 0x80 + 1);
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
    sprintf(ArcName, "multi(0)disk(0)rdisk(%u)", DriveNumber - 0x80);
    AddReactOSArcDiskInfo(ArcName, Signature, Checksum, ValidPartitionTable);

    sprintf(ArcName, "multi(0)disk(0)rdisk(%u)partition(0)", DriveNumber - 0x80);
    FsRegisterDevice(ArcName, &UefiDiskVtbl);

    /* Add partitions */
    i = 1;
    while (DiskGetPartitionEntry(DriveNumber, i, &PartitionTableEntry))
    {
        if (PartitionTableEntry.SystemIndicator != PARTITION_ENTRY_UNUSED)
        {
            sprintf(ArcName, "multi(0)disk(0)rdisk(%u)partition(%lu)", DriveNumber - 0x80, i);
            FsRegisterDevice(ArcName, &UefiDiskVtbl);
        }
        i++;
    }

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

VOID
UefiSetupBlockDevices()
{
    ULONG i;
    ULONG SystemHandleCount;

    ULONG Count = 0;
    EFI_STATUS Status = 0;
    OffsetToBoot = 0;
    UefiBootRootIdentifier = 0;
    TRACE("UefiSetupBlockDevices: Called\n");

    /* 1) Setup a list of boothandles by using the LocateHandle protocol */
    Status = GlobalSystemTable->BootServices->LocateHandle(ByProtocol, &bioGuid, NULL, &handle_size, handles);
    do
    {   Count += 1;
        if (handles)
            MmFreeMemory(handles);
        handles = MmAllocateMemoryWithType((handle_size + (EFI_PAGE_SIZE * Count)), LoaderFirmwareTemporary);
        Status = GlobalSystemTable->BootServices->LocateHandle(ByProtocol, &bioGuid, NULL, &handle_size, handles);
    } while(Status != STATUS_SUCCESS);

    SystemHandleCount = handle_size / sizeof(EFI_HANDLE);

    /* 2) Parse through block devicel list */
    for (i = 0; i <= SystemHandleCount; ++i) {
        Status = GlobalSystemTable->BootServices->HandleProtocol(handles[i], &bioGuid, (void **) &bio);
        if (EFI_ERROR(Status) || bio == NULL || bio->Media->BlockSize==0){
            TRACE("UefiSetupBlockDevices: UEFI Has found a block device thats failed, Skipping\n");
            continue;
        }
        else
        {
            TRACE("UefiSetupBlockDevices: Found a block device with size %d\n", bio->Media->BlockSize);
        }
        if (handles[i] == PublicBootHandle)
        {
            OffsetToBoot = i;
            TRACE("\nUefiSetupBlockDevices: Found the BootHandle, Array Offset:%d\n", OffsetToBoot);
        }
        // For every device, make sure it's a Root Block device
        if (bio->Media->LogicalPartition == FALSE)
        {
            PcBiosDiskCount += 1;
            TRACE("UefiSetupBlockDevices: Found Root of drive %d\nSectorSize: %d\n", i, bio->Media->BlockSize);
            GetHarddiskInformation(i + FIRST_BIOS_DISK);

        }
        else if(handles[i] == PublicBootHandle)
        {

            ULONG increment = 0;
            for(increment = OffsetToBoot; increment >= 0; increment--)
            {
                GlobalSystemTable->BootServices->HandleProtocol(handles[increment], &bioGuid, (void **) &bio);
                if (bio->Media->LogicalPartition == FALSE)
                {
                    TRACE("Found root at increment %u", increment);
                    UefiBootRootIdentifier = increment;
                    break;
                }
            }

            //HACK: Sometimes UEFI doesn't define the Root drive properly
        }
    }
}

BOOLEAN
UefiSetBootpath()
{
   TRACE("UefiSetBootpath: Setting up boot path\n");
   GlobalSystemTable->BootServices->HandleProtocol(handles[UefiBootRootIdentifier], &bioGuid, (void **) &bio);
   FrldrBootDrive = (FIRST_BIOS_DISK + UefiBootRootIdentifier);
   if (bio->Media->RemovableMedia == TRUE && bio->Media->BlockSize == 2048)
   {
        FrldrBootPartition == 0xFF;
           /* Boot Partition 0xFF is the magic value that indicates booting from CD-ROM (see isoboot.S) */
        RtlStringCbPrintfA(FrLdrBootPath, sizeof(FrLdrBootPath),
                           "multi(0)disk(0)cdrom(%u)", UefiBootRootIdentifier);
   }
   else
   {
        ULONG BootPartition;
        PARTITION_TABLE_ENTRY PartitionEntry;

        /* This is a hard disk */
        if (!UefiGetBootPartitionEntry(FrldrBootDrive, &PartitionEntry, &BootPartition))
        {
            ERR("Failed to get boot partition entry\n");
            return FALSE;
        }

        RtlStringCbPrintfA(FrLdrBootPath, sizeof(FrLdrBootPath),
                           "multi(0)disk(0)rdisk(%u)partition(%lu)",
                           UefiBootRootIdentifier, BootPartition);
    }

    return TRUE;
}

BOOLEAN
UefiInitializeBootDevices(VOID)
{
    ULONG i;
    TRACE("UefiInitializeBootDevices: Called\n");
    DiskReadBufferSize = PAGE_SIZE;
    DiskReadBuffer = MmAllocateMemoryWithType(DiskReadBufferSize, LoaderFirmwareTemporary);
    UefiSetupBlockDevices();
    UefiSetBootpath();

    /* Add it, if it's a floppy or cdrom */
    GlobalSystemTable->BootServices->HandleProtocol(handles[UefiBootRootIdentifier], &bioGuid, (void **) &bio);
    if (bio->Media->RemovableMedia == TRUE)
    {
        /* TODO: Check if it's really a CDROM drive */

        PMASTER_BOOT_RECORD Mbr;
        PULONG Buffer;
        ULONG Checksum = 0;
        ULONG Signature;

        /* Read the MBR */
        if (!MachDiskReadLogicalSectors(FrldrBootDrive, 16ULL, 1, DiskReadBuffer))
        {
            ERR("Reading MBR failed\n");
            return FALSE;
        }

        Buffer = (ULONG*)DiskReadBuffer;
        Mbr = (PMASTER_BOOT_RECORD)DiskReadBuffer;

        Signature = Mbr->Signature;
        TRACE("Signature: %x\n", Signature);

        /* Calculate the MBR checksum */
        for (i = 0; i < 2048 / sizeof(ULONG); i++)
        {
            Checksum += Buffer[i];
        }
        Checksum = ~Checksum + 1;
        TRACE("Checksum: %x\n", Checksum);

        /* Fill out the ARC disk block */
        AddReactOSArcDiskInfo(FrLdrBootPath, Signature, Checksum, TRUE);

        FsRegisterDevice(FrLdrBootPath, &UefiDiskVtbl);
       // DiskCount++; // This is not accounted for in the number of pre-enumerated BIOS drives!
        TRACE("Additional boot drive detected: 0x%02X\n", (int)FrldrBootDrive);
    }
    return TRUE;
}

UCHAR
UefiGetFloppyCount(VOID)
{
    /* no floppy for you */
    return 0;
}

BOOLEAN
UefiDiskReadLogicalSectors(
    IN UCHAR DriveNumber,
    IN ULONGLONG SectorNumber,
    IN ULONG SectorCount,
    OUT PVOID Buffer)
{
    PVOID mbr[2048];
    ULONG UefiDriveNumber = 0;
    UefiDriveNumber = (DriveNumber - FIRST_BIOS_DISK);
    TRACE("UefiDiskReadLogicalSectors: DriveNumber: %d\n", UefiDriveNumber);
    GlobalSystemTable->BootServices->HandleProtocol(handles[UefiDriveNumber], &bioGuid, (void **) &bio);

    /* Devices setup */
    bio->ReadBlocks(bio, bio->Media->MediaId, SectorNumber, (bio->Media->BlockSize * SectorCount),  &mbr);
    RtlCopyMemory(Buffer, mbr, SectorCount * bio->Media->BlockSize);
    return TRUE;
}

BOOLEAN
UefiDiskGetDriveGeometry(UCHAR DriveNumber, PGEOMETRY Geometry)
{
    ULONG UefiDriveNumber = 0;
    TRACE("UefiDiskGetDriveGeometry: DriveNumber: %d\n", DriveNumber);
    UefiDriveNumber = (DriveNumber - FIRST_BIOS_DISK);
    GlobalSystemTable->BootServices->HandleProtocol(handles[UefiDriveNumber], &bioGuid, (void **) &bio);
    Geometry->Cylinders = 1;      // Number of cylinders on the disk
    Geometry->Heads = 1;          // Number of heads on the disk
    Geometry->Sectors = bio->Media->LastBlock;        // Number of sectors per track
    Geometry->BytesPerSector = bio->Media->BlockSize; // Number of bytes per sector

    /* Nothing ..? */
    return TRUE;
}

ULONG
UefiDiskGetCacheableBlockCount(UCHAR DriveNumber)
{
    ULONG UefiDriveNumber = 0;
    UefiDriveNumber = (DriveNumber - FIRST_BIOS_DISK);
    TRACE("UefiDiskGetCacheableBlockCount: DriveNumber: %d\n", UefiDriveNumber);

    GlobalSystemTable->BootServices->HandleProtocol(handles[UefiDriveNumber], &bioGuid, (void **) &bio);
    return bio->Media->LastBlock;
}
