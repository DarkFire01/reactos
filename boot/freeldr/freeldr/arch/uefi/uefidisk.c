#include <uefildr.h>


#include <debug.h>
//DBG_DEFAULT_CHANNEL(WARNING);
extern EFI_SYSTEM_TABLE * GlobalSystemTable;
extern EFI_HANDLE GlobalImageHandle;

#define TAG_HW_RESOURCE_LIST    'lRwH'
#define TAG_HW_DISK_CONTEXT     'cDwH'
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
      //  printf("UefiDiskGetFileInformation\r\n");
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
    DISKCONTEXT* Context;
  //  UCHAR DriveNumber = 0;
    //ULONG DrivePartition;
    ULONG SectorSize;
    ULONGLONG SectorOffset = 0;
    ULONGLONG SectorCount = 0;
 //   PARTITION_TABLE_ENTRY PartitionTableEntry;
     //   printf("UefiDiskOpe n\r\n");
    SectorSize = bio->Media->BlockSize ;
    GEOMETRY Geometry;
    if (!MachDiskGetDriveGeometry(0, &Geometry))
        return EINVAL;
    if (SectorSize != Geometry.BytesPerSector)
    {
        printf("SectorSize (%lu) != Geometry.BytesPerSector (%lu), expect problems!\n",
            SectorSize, Geometry.BytesPerSector);
    }
    SectorOffset = 0;
    SectorCount = (ULONGLONG)Geometry.Cylinders * Geometry.Heads * Geometry.Sectors;

    Context = FrLdrTempAlloc(sizeof(DISKCONTEXT), TAG_HW_DISK_CONTEXT);
    if (!Context)
        return ENOMEM;

    Context->DriveNumber = 0;
    Context->SectorSize = SectorSize;
    Context->SectorOffset = SectorOffset;
    Context->SectorCount = SectorCount;
    Context->SectorNumber = 0;
    FsSetDeviceSpecific(*FileId, Context);
    printf("UefiDiskOpen : Sucess\r\n");
    return ESUCCESS;
}

static ARC_STATUS
UefiDiskRead(ULONG FileId, VOID *Buffer, ULONG N, ULONG *Count)
{
    DISKCONTEXT* Context = FsGetDeviceSpecific(FileId);
    UCHAR* Ptr = (UCHAR*)Buffer;
    ULONG Length, TotalSectors, MaxSectors, ReadSectors;
    ULONGLONG SectorOffset;
    BOOLEAN ret;


    TotalSectors = (N + Context->SectorSize - 1) / Context->SectorSize;
    MaxSectors   = DiskReadBufferSize / Context->SectorSize;
    SectorOffset = Context->SectorOffset + Context->SectorNumber;
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
   // printf("UefiDiskSeek\r\n");
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
//    printf("UefiDiskSeek : Success \r\n");
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

BOOLEAN
UefiGetBootPartitionEntry(
    IN UCHAR DriveNumber,
    OUT PPARTITION_TABLE_ENTRY PartitionTableEntry,
    OUT PULONG BootPartition)
    {
        BootPartition = 0;
        return TRUE;
    }
extern EFI_HANDLE PublicBootHandle;
static const CHAR Hex[] = "0123456789abcdef";
static CHAR PcDiskIdentifier[32][20];
VOID
UefiSetupBlockDevices()
{
    PMASTER_BOOT_RECORD Mbr;
    EFI_STATUS status;
    PULONG Buffer;
    BOOLEAN ValidPartitionTable;
    ULONG i;
    ULONG Checksum;
    ULONG Signature;
    CHAR ArcName[MAX_PATH];
    PCHAR Identifier = PcDiskIdentifier[0];

    GlobalSystemTable->BootServices->LocateHandle(ByProtocol, &bioGuid, NULL, &handle_size, handles);
    handles = MmAllocateMemoryWithType((handle_size + EFI_PAGE_SIZE - 1 )/ EFI_PAGE_SIZE, LoaderFirmwareTemporary);
    GlobalSystemTable->BootServices->LocateHandle(ByProtocol, &bioGuid, NULL, &handle_size, handles);
    for (ULONG i = 0; i < handle_size / sizeof(EFI_HANDLE); i++) {
    status = GlobalSystemTable->BootServices->HandleProtocol(handles[i], &bioGuid, (void **) &bio);
    /* if unsuccessful, skip and go over to the next device */
    if (EFI_ERROR(status) || bio == NULL || bio->Media->BlockSize==0){
            printf("Failed");
            continue;
        }
        else{
            printf("Sucess!, Block size: %d\r\n", bio->Media->BlockSize);
        }
    }

    status = GlobalSystemTable->BootServices->HandleProtocol(PublicBootHandle, &bioGuid, (void **) &bio);

    /* Read the MBR */
    if (!MachDiskReadLogicalSectors(0, 0ULL, 1, DiskReadBuffer))
    {
        printf("Reading MBR failed\n");

    }

    Buffer = (ULONG*)DiskReadBuffer;
    Mbr = (PMASTER_BOOT_RECORD)DiskReadBuffer;

    Signature = Mbr->Signature;

    /* Calculate the MBR checksum */
    Checksum = 0;
    for (i = 0; i < 2048 / sizeof(ULONG); i++)
    {
        Checksum += Buffer[i];
    }
    Checksum = ~Checksum + 1;
    printf("Checksum: %x\n", Checksum);
    ValidPartitionTable = (Mbr->MasterBootRecordMagic == 0xAA55);
    if (Mbr->MasterBootRecordMagic == 0xAA55)
    {
        printf("Detected MBR disk");
    }

    /* Fill out the ARC disk block */
    sprintf(ArcName, "multi(0)disk(0)rdisk(%u)",0);
    AddReactOSArcDiskInfo(ArcName, Signature, Checksum, ValidPartitionTable);

    sprintf(ArcName, "multi(0)disk(0)rdisk(%u)partition(0)", 0);
    FsRegisterDevice(ArcName, &UefiDiskVtbl);

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
 //   UefiConsSetCursor(0,0);
}

BOOLEAN
UefiSetBootpath()
{
    if (*FrLdrBootPath)
        return TRUE;

    /* 0x49 is our magic ramdisk drive, so try to detect it first */
    if (FrldrBootDrive == 0x49)
    {
        /* This is the ramdisk. See ArmInitializeBootDevices() too... */
        // RtlStringCbPrintfA(FrLdrBootPath, sizeof(FrLdrBootPath), "ramdisk(%u)", 0);
        RtlStringCbCopyA(FrLdrBootPath, sizeof(FrLdrBootPath), "ramdisk(0)");
    }
    else if (FrldrBootDrive < 0x80)
    {
        /* This is a floppy */
        RtlStringCbPrintfA(FrLdrBootPath, sizeof(FrLdrBootPath),
                           "multi(0)disk(0)fdisk(%u)", FrldrBootDrive);
    }
    else if (FrldrBootPartition == 0xFF)
    {
        /* Boot Partition 0xFF is the magic value that indicates booting from CD-ROM (see isoboot.S) */
        RtlStringCbPrintfA(FrLdrBootPath, sizeof(FrLdrBootPath),
                           "multi(0)disk(0)cdrom(%u)", FrldrBootDrive - 0x80);
    }
    else
    {
        ULONG BootPartition;
        PARTITION_TABLE_ENTRY PartitionEntry;

        /* This is a hard disk */
        if (!UefiGetBootPartitionEntry(FrldrBootDrive, &PartitionEntry, &BootPartition))
        {
            printf("Failed to get boot partition entry\n");
            return FALSE;
        }

        //FrldrBootPartition = BootPartition;
        FrldrBootPartition = 0;
        RtlStringCbPrintfA(FrLdrBootPath, sizeof(FrLdrBootPath),
                           "multi(0)disk(0)rdisk(%u)partition(%lu)",
                           FrldrBootDrive - 0x80, FrldrBootPartition);
    }
    return TRUE;
}

BOOLEAN
UefiInitializeBootDevices(VOID)
{
     DiskReadBufferSize = PAGE_SIZE;

    DiskReadBuffer = MmAllocateMemoryWithType(DiskReadBufferSize, LoaderFirmwareTemporary);
    UefiVideoClearScreen(0);
    UefiSetupBlockDevices();
    FrldrBootDrive = 0x80;
    UefiSetBootpath();
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

    /* Devices setup */
    bio->ReadBlocks(bio, bio->Media->MediaId, SectorNumber, (bio->Media->BlockSize * SectorCount),  &mbr);
    RtlCopyMemory(Buffer, mbr, SectorCount * bio->Media->BlockSize);

    return TRUE;
}

BOOLEAN
UefiDiskGetDriveGeometry(UCHAR DriveNumber, PGEOMETRY Geometry)
{
    /* Nothing ..? */

    Geometry->Cylinders = 1;      // Number of cylinders on the disk
    Geometry->Heads = 1;          // Number of heads on the disk
    Geometry->Sectors = bio->Media->LastBlock;        // Number of sectors per track
    Geometry->BytesPerSector = bio->Media->BlockSize; // Number of bytes per sector

    return TRUE;
}

ULONG
UefiDiskGetCacheableBlockCount(UCHAR DriveNumber)
{
    return bio->Media->LastBlock;
}
