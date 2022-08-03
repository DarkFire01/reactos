#include <uefildr.h>


#include <debug.h>
DBG_DEFAULT_CHANNEL(WARNING);

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

BOOLEAN
UefiInitializeBootDevices(VOID)
{

    UefiVideoClearScreen(0);
   // UefiPrintF("Starting up UEFI Disk access\r\n", 1, 1, 0xFFFFFF, 0x000000);
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
    printf("DiskReadLogicalSectors: Sector Count %d Drive Number: %d", SectorCount, DriveNumber);

    return FALSE;
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

typedef struct tagFILEDATA
{
    ULONG DeviceId;
    ULONG ReferenceCount;
    const DEVVTBL* FuncTable;
    const DEVVTBL* FileFuncTable;
    VOID* Specific;
} FILEDATA; 

typedef struct tagDEVICE
{
    LIST_ENTRY ListEntry;
    const DEVVTBL* FuncTable;
    CHAR* Prefix;
    ULONG DeviceId;
    ULONG ReferenceCount;
} DEVICE;

const DEVVTBL* UefiMount(ULONG DeviceId)
{
     UefiVideoClearScreen(0);
       UefiPrintF("ARC opened", 1, 1, 0xFFFFFF, 0x000000);

    /* lol what is this */
    for(;;)
    {

    }
    return NULL;
}
static FILEDATA FileData[MAX_FDS];
static LIST_ENTRY DeviceListHead;
#define TAG_DEVICE_NAME 'NDsF'
#define TAG_DEVICE 'vDsF'
#if 0 
/* File ID is literally just the fucking offset of the FileData Array */
ARC_STATUS ArcOpen(CHAR* Path, OPENMODE OpenMode, ULONG* FileId)
{
       UefiVideoClearScreen(0);
       UefiPrintF("Opening ARC", 1, 1, 0xFFFFFF, 0x000000);

    ARC_STATUS Status;
    ULONG Count, i;
    PLIST_ENTRY pEntry;
    DEVICE* pDevice;
    CHAR* DeviceName;
    CHAR* FileName;
    CHAR* p;
    CHAR* q;
    SIZE_T Length;
    OPENMODE DeviceOpenMode;
    ULONG DeviceId;

    /* Print status message */
   // TRACE("Opening file '%s'...\n", Path);

    *FileId = MAX_FDS;

    /* Search last ')', which delimits device and path */
    FileName = strrchr(Path, ')');
    if (!FileName)
        return EINVAL;
    FileName++;

    /* Count number of "()", which needs to be replaced by "(0)" */
    Count = 0;
    for (p = Path; p != FileName; p++)
    {
        if (*p == '(' && *(p + 1) == ')')
            Count++;
    }

    /* Duplicate device name, and replace "()" by "(0)" (if required) */
    Length = FileName - Path + Count;
    if (Count != 0)
    {
        DeviceName = FrLdrTempAlloc(FileName - Path + Count, TAG_DEVICE_NAME);
        if (!DeviceName)
            return ENOMEM;
        for (p = Path, q = DeviceName; p != FileName; p++)
        {
            *q++ = *p;
            if (*p == '(' && *(p + 1) == ')')
                *q++ = '0';
        }
    }
    else
    {
        DeviceName = Path;
    }

    /* Search for the device */
    if (OpenMode == OpenReadOnly || OpenMode == OpenWriteOnly)
        DeviceOpenMode = OpenMode;
    else
        DeviceOpenMode = OpenReadWrite;

    pEntry = DeviceListHead.Flink;
    while (pEntry != &DeviceListHead)
    {
        pDevice = CONTAINING_RECORD(pEntry, DEVICE, ListEntry);
        if (strncmp(pDevice->Prefix, DeviceName, Length) == 0)
        {
            /* OK, device found. It is already opened? */
            if (pDevice->ReferenceCount == 0)
            {
                /* Search some room for the device */
                for (DeviceId = 0; DeviceId < MAX_FDS; DeviceId++)
                {
                    if (!FileData[DeviceId].FuncTable)
                        break;
                }
                if (DeviceId == MAX_FDS)
                    return EMFILE;

                /* Try to open the device */
                FileData[DeviceId].FuncTable = pDevice->FuncTable;
                Status = pDevice->FuncTable->Open(pDevice->Prefix, DeviceOpenMode, &DeviceId);
                if (Status != ESUCCESS)
                {
                    FileData[DeviceId].FuncTable = NULL;
                    return Status;
                }
                else if (!*FileName)
                {
                    /* Done, caller wanted to open the raw device */
                    *FileId = DeviceId;
                    pDevice->ReferenceCount++;
                    return ESUCCESS;
                }

                /* Try to detect the file system */
                FileData[DeviceId].FileFuncTable = UefiMount(DeviceId);
                if (!FileData[DeviceId].FileFuncTable)

                if (!FileData[DeviceId].FileFuncTable)
                {
                    /* Error, unable to detect file system */
                    pDevice->FuncTable->Close(DeviceId);
                    FileData[DeviceId].FuncTable = NULL;
                    return ENODEV;
                }

                pDevice->DeviceId = DeviceId;
            }
            else
            {
                DeviceId = pDevice->DeviceId;
            }
            pDevice->ReferenceCount++;
            break;
        }
        pEntry = pEntry->Flink;
    }
    if (pEntry == &DeviceListHead)
        return ENODEV;

    /* At this point, device is found and opened. Its file id is stored
     * in DeviceId, and FileData[DeviceId].FileFuncTable contains what
     * needs to be called to open the file */

    /* Search some room for the device */
    for (i = 0; i < MAX_FDS; i++)
    {
        if (!FileData[i].FuncTable)
            break;
    }
    if (i == MAX_FDS)
        return EMFILE;

    /* Skip leading path separator, if any */
    if (*FileName == '\\' || *FileName == '/')
        FileName++;

    /* Open the file */
    FileData[i].FuncTable = FileData[DeviceId].FileFuncTable;
    FileData[i].DeviceId = DeviceId;
    *FileId = i;
    Status = FileData[i].FuncTable->Open(FileName, OpenMode, FileId);
    if (Status != ESUCCESS)
    {
        FileData[i].FuncTable = NULL;
        *FileId = MAX_FDS;
    }
    return Status;
}

#endif