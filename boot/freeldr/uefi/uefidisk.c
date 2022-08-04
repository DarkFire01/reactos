#include <uefildr.h>


#include <debug.h>
DBG_DEFAULT_CHANNEL(WARNING);

typedef struct tagDISKCONTEXT
{
    UCHAR DriveNumber;
    ULONG SectorSize;
    ULONGLONG SectorOffset;
    ULONGLONG SectorCount;
    ULONGLONG SectorNumber;
} DISKCONTEXT;

PVOID DiskReadBuffer;
SIZE_T DiskReadBufferSize;
//struct EFI_GUID EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID = {0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};

EFI_SYSTEM_TABLE* LocSystemTable;
EFI_HANDLE LocImageHandle;
//EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Volume;
//EFI_FILE_PROTOCOL* RootFS;
PVOID Buffer;
UINT64* LoaderFileSize;

//BLOCKINFO bi;
VOID
UefiInitializeFileSystemSupport(_In_ EFI_HANDLE ImageHandle,
                                _In_ EFI_SYSTEM_TABLE *SystemTable)
{
   // EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
	//EFI_DEVICE_PATH_PROTOCOL *DevicePath;
    //EFI_STATUS RefiStatus;

    LocSystemTable = SystemTable;
    LocImageHandle = ImageHandle;
#if 0

    RefiStatus = SystemTable->BootServices->HandleProtocol(ImageHandle, &EFI_LOADED_IMAGE_PROTOCOL_GUID, (PVOID)&LoadedImage);
    if(RefiStatus != EFI_SUCCESS)
    {
    }
    RefiStatus = SystemTable->BootServices->HandleProtocol(LoadedImage->DeviceHandle, &EFI_DEVICE_PATH_PROTOCOL_GUID, (PVOID)&DevicePath);
	if(RefiStatus != EFI_SUCCESS)
    {
    }
    RefiStatus = SystemTable->BootServices->HandleProtocol(LoadedImage->DeviceHandle, &EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID, (PVOID)&Volume);
	if(RefiStatus != EFI_SUCCESS)
    {
    }
    RefiStatus = Volume->OpenVolume(Volume, &RootFS);
#endif
}

static ARC_STATUS
UefiDiskClose(ULONG FileId)
{
    for(;;)
    {
        printf("UefiDiskClose");
    }

    return ESUCCESS;
}

static ARC_STATUS
UefiDiskGetFileInformation(ULONG FileId, FILEINFORMATION *Information)
{
    for(;;)
    {
        printf("UefiDiskGetFileInformation");
    }

    return ESUCCESS;
}

static ARC_STATUS
UefiDiskOpen(CHAR *Path, OPENMODE OpenMode, ULONG *FileId)
{
    for(;;)
    {
        printf("UefiDiskOpen");
    }

    return ESUCCESS;
}

static ARC_STATUS
UefiDiskRead(ULONG FileId, VOID *Buffer, ULONG N, ULONG *Count)
{
    for(;;)
    {
        printf("UefiDiskRead");
    }

    return ESUCCESS;
}

static ARC_STATUS
UefiDiskSeek(ULONG FileId, LARGE_INTEGER *Position, SEEKMODE SeekMode)
{
    for(;;)
    {
        printf("UefiDiskSeek");
    }

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


VOID
UefiSetupBlockDevices()
{
    PMASTER_BOOT_RECORD Mbr;
    PULONG Buffer;
    ULONG i;
    ULONG Checksum;
    ULONG Signature;
    /* Now lets play around */
    printf("Attempting read\r\n");

        /* Read the MBR */
    if (!MachDiskReadLogicalSectors(0, 0ULL, 1, DiskReadBuffer))
    {
        printf("Reading MBR failed\n");

    }

    Buffer = (ULONG*)DiskReadBuffer;
    Mbr = (PMASTER_BOOT_RECORD)DiskReadBuffer;

    Signature = Mbr->Signature;
    printf("Signature: %x\n", Signature);

    /* Calculate the MBR checksum */
    Checksum = 0;
    for (i = 0; i < 512 / sizeof(ULONG); i++)
    {
        Checksum += Buffer[i];
    }
    Checksum = ~Checksum + 1;
    printf("Checksum: %x\n", Checksum);

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
    UefiVideoClearScreen(0);
    UefiPrintF("Starting up UEFI Disk access\r\n", 1, 1, 0xFFFFFF, 0x000000);
    UefiSetupBlockDevices();
    for(;;)
    {

    }
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


EFI_GUID bioGuid = BLOCK_IO_PROTOCOL;
EFI_BLOCK_IO *bio;
EFI_HANDLE *handles = NULL;
UINTN handle_size = 0;
BOOLEAN
UefiDiskReadLogicalSectors(
    IN UCHAR DriveNumber,
    IN ULONGLONG SectorNumber,
    IN ULONG SectorCount,
    OUT PVOID Buffer)
{
    EFI_STATUS status;
        ULONG Signature;
            ULONG Checksum;
    PMASTER_BOOT_RECORD MbrCheck;
    printf("DiskReadLogicalSectors: Sector Count %d Drive Number: %d\r\n", SectorCount, DriveNumber);
    LocSystemTable->BootServices->LocateHandle(ByProtocol, &bioGuid, NULL, &handle_size, handles);
    handles = MmAllocateMemoryWithType((handle_size + EFI_PAGE_SIZE - 1 )/ EFI_PAGE_SIZE, LoaderFirmwareTemporary);
    LocSystemTable->BootServices->LocateHandle(ByProtocol, &bioGuid, NULL, &handle_size, handles);
    for (ULONG i = 0; i < handle_size / sizeof(EFI_HANDLE); i++) {
    status = LocSystemTable->BootServices->HandleProtocol(handles[i], &bioGuid, (void **) &bio);
    /* if unsuccessful, skip and go over to the next device */
    if (EFI_ERROR(status) || bio == NULL || bio->Media->BlockSize==0){
            printf("Failed");
            continue;
        }
        else{
            printf("Sucess!, Block size: %d\r\n", bio->Media->BlockSize);
        }
    }


    status = LocSystemTable->BootServices->HandleProtocol(handles[1], &bioGuid, (void **) &bio);

    /* TODO: do something with that device. */

    #if 0
    IN EFI_BLOCK_IO_PROTOCOL          *This,
  IN UINT32                         MediaId,
  IN EFI_LBA                        Lba,
  IN UINTN                          BufferSize,
  OUT VOID                          *Buffer
    #endif
    UINT8 mbr[512];
    /* Devices setup */
    bio->ReadBlocks(bio, bio->Media->MediaId, SectorCount - 1, (bio->Media->BlockSize * SectorCount),  &mbr);

    MbrCheck = (PMASTER_BOOT_RECORD)mbr;
    Signature = MbrCheck->Signature;
    printf("Signature: %X\n", Signature);

       /* Calculate the MBR checksum */
    Checksum = 0;
    for (ULONG i = 0; i < 512 / sizeof(ULONG); i++)
    {
        Checksum += mbr[i];
    }
    Checksum = ~Checksum + 1;
    printf("Checksum: %x\n", Checksum);
    printf("MBR magic: %X\n", MbrCheck->MasterBootRecordMagic);
    RtlCopyMemory(Buffer, mbr, SectorCount * bio->Media->BlockSize);

    return TRUE;
}

BOOLEAN
UefiDiskGetDriveGeometry(UCHAR DriveNumber, PGEOMETRY Geometry)
{
    /* Nothing ..? */
    return TRUE;
}

ULONG
UefiDiskGetCacheableBlockCount(UCHAR DriveNumber)
{
    return 0;
}
