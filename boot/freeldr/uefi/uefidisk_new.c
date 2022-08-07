/*
 * PROJECT:     Freeldr UEFI Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI Disk access function
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

/**** Includes ***********************************************************************************/

#include <uefildr.h>

#include <debug.h>
DBG_DEFAULT_CHANNEL(WARNING);

/**** Global Variables ***************************************************************************/

extern EFI_SYSTEM_TABLE* LocSystemTable;
extern SIZE_T DiskReadBufferSize;
extern EFI_HANDLE LocImageHandle;
extern ULONG FrldrBootPartition;
extern UCHAR FrldrBootDrive;

UINT64* LoaderFileSize;
EFI_BLOCK_IO *bio;
PVOID Buffer;

EFI_GUID bioGuid = BLOCK_IO_PROTOCOL;
EFI_HANDLE *handles = NULL;
UINTN handle_size = 0;

/**** Private Functions **************************************************************************/

/*
 * We need some block handles setup before we deal with anything.
 * Here we have UEFI specific, while rest goes through this.
 * Unfortantely: UEFI can be a bit... odd about this, when we deal with
 * secure boot I've noticed sometimes we can't setup raw block devices
 * We handle that here. Freeldr NEEDS to have raw block access.
 */
EFI_STATUS
PREFI_SetupDiskAccess()
{
	EFI_STATUS status;

    status = LocSystemTable->BootServices->LocateHandle(ByProtocol, &bioGuid, 
														NULL, &handle_size, handles);

    handles = MmAllocateMemoryWithType((handle_size + EFI_PAGE_SIZE - 1 ) / EFI_PAGE_SIZE,
										LoaderFirmwareTemporary);
    
	status = LocSystemTable->BootServices->LocateHandle(ByProtocol, &bioGuid, 
														NULL, &handle_size, handles);

	if (status != EFI_SUCCESS)
	{
		//TODO: Fail
		return status;
	}

    for (ULONG i = 0; i < handle_size / sizeof(EFI_HANDLE); i++)
	{
		status = LocSystemTable->BootServices->HandleProtocol(handles[i], &bioGuid, (void **) &bio);

		/* if unsuccessful, skip and go over to the next device */
		if (EFI_ERROR(status) || bio == NULL || bio->Media->BlockSize==0)
		{
            continue;
        }
        else
		{
            printf("Found a storage device! Block size: %d\r\n", bio->Media->BlockSize);
        }
    }
}

/**** Public Functions ***************************************************************************/

static const DEVVTBL UefiDiskVtbl =
{
    UefiDiskClose,
    UefiDiskGetFileInformation,
    UefiDiskOpen,
    UefiDiskRead,
    UefiDiskSeek,
};

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
	LocSystemTable->BootServices->HandleProtocol(handles[DriveNumber], &bioGuid, (void **) &bio);

    bio->ReadBlocks(bio, bio->Media->MediaId, SectorNumber, (bio->Media->BlockSize * SectorCount),  &mbr);
    RtlCopyMemory(Buffer, mbr, SectorCount * bio->Media->BlockSize);

    return TRUE;
}
BOOLEAN
UefiDiskGetDriveGeometry(UCHAR DriveNumber, PGEOMETRY Geometry)
{
	LocSystemTable->BootServices->HandleProtocol(handles[DriveNumber], &bioGuid, (void **) &bio);

    Geometry->Cylinders = 1;      // Number of cylinders on the disk
    Geometry->Heads = 1;          // Number of heads on the disk
    Geometry->Sectors = bio->Media->LastBlock;        // Number of sectors per track
    Geometry->BytesPerSector = bio->Media->BlockSize; // Number of bytes per sector
    
    return TRUE;
}

ULONG
UefiDiskGetCacheableBlockCount(UCHAR DriveNumber)
{  
    LocSystemTable->BootServices->HandleProtocol(handles[DriveNumber], &bioGuid, (void **) &bio);
    return bio->Media->LastBlock;
}
