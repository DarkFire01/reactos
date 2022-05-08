#include <uefildr.h>


#include <debug.h>
DBG_DEFAULT_CHANNEL(WARNING);

EFI_SYSTEM_TABLE* LocSystemTable;
EFI_HANDLE LocImageHandle;

VOID
UefiInitializeFileSystemSupport(_In_ EFI_HANDLE ImageHandle,
                                _In_ EFI_SYSTEM_TABLE *SystemTable)
{
    LocSystemTable = SystemTable;
    LocImageHandle = ImageHandle;
}

BOOLEAN
UefiInitializeBootDevices(VOID)
{

    UefiVideoClearScreen(0);
    UefiPrintF("Starting up UEFI Disk access", 1, 1, 0xFFFFFF, 0x000000);
    for(;;)
    {

    }
    return FALSE;
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
    return FALSE;
}

BOOLEAN
UefiDiskGetDriveGeometry(UCHAR DriveNumber, PGEOMETRY Geometry)
{
    return FALSE;
}

ULONG
UefiDiskGetCacheableBlockCount(UCHAR DriveNumber)
{
    return 0;
}