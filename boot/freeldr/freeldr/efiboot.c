/*
 * PROJECT:     FreeLoader UEFI Support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     EFI Chainloading support
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

/* INCLUDES *******************************************************************/

#include <uefildr.h>

#include <debug.h>
DBG_DEFAULT_CHANNEL(WARNING);

/* GLOBALS ********************************************************************/

PVOID   EfiAppLoadAddress = NULL;
extern EFI_HANDLE GlobalImageHandle;
extern EFI_SYSTEM_TABLE *GlobalSystemTable;
#define LINUX_READ_CHUNK_SIZE   0x10000 // Read 128k at a time
/* FUNCTIONS ******************************************************************/

EFI_STATUS
StartEfiFile(UINTN* memoryLocation, UINTN fileSize)
{
	EFI_STATUS status;
	EFI_HANDLE newImageHandle = NULL;


	if (memoryLocation == NULL)
	{
		ERR("memoryLocation or fileSize is NULL\n");
		return EFI_INVALID_PARAMETER;
	}

	status = GlobalSystemTable->BootServices->LoadImage(TRUE,
		                                                GlobalImageHandle,
		                                                NULL,
		                                                memoryLocation,
		                                                fileSize,
		                                                &newImageHandle);

	if (status != EFI_SUCCESS)
	{
		switch (status)
		{
		    case EFI_OUT_OF_RESOURCES:
		    	ERR("EFI_OUT_OF_RESOURCES\n");
		    	break;
		    case EFI_INVALID_PARAMETER:
		    	ERR("EFI_INVALID_PARAMETER\n");
		    	break;
		    case EFI_NOT_FOUND:
		    	ERR("EFI_NOT_FOUND\n");
		    	break;
		    case EFI_UNSUPPORTED:
		    	ERR("EFI_UNSPPORTED\n");
		    	break;
		    case EFI_LOAD_ERROR:
		    	ERR("EFI_LOAD_ERROR\n");
		    	break;
		    case EFI_DEVICE_ERROR:
		    	ERR("EFI_DEVICE_ERROR\n");
		    	break;
		    default:
		    	break;
		}
		ERR("Failed at LoadImage!\n");
		return status;
	}
    TRACE("Starting IMAGE\n");
	status = GlobalSystemTable->BootServices->StartImage(newImageHandle,
		                                                 NULL,
		                                                 NULL);

	if (status != EFI_SUCCESS)
	{
        if (status != EFI_SUCCESS)
	{
		switch (status)
		{
		    case EFI_OUT_OF_RESOURCES:
		    	ERR("EFI_OUT_OF_RESOURCES\n");
		    	break;
		    case EFI_INVALID_PARAMETER:
		    	ERR("EFI_INVALID_PARAMETER\n");
		    	break;
		    case EFI_NOT_FOUND:
		    	ERR("EFI_NOT_FOUND\n");
		    	break;
		    case EFI_UNSUPPORTED:
		    	ERR("EFI_UNSPPORTED\n");
		    	break;
		    case EFI_LOAD_ERROR:
		    	ERR("EFI_LOAD_ERROR\n");
		    	break;
		    case EFI_DEVICE_ERROR:
		    	ERR("EFI_DEVICE_ERROR\n");
		    	break;
		    default:
		    	break;
		}
				ERR("Failed at StartImage\n");
		return status;
	}
		return status;
	}

	return status;
}

static BOOLEAN EfiReadApp(ULONG FileId, PCSTR EfiAppName, UINT32 EfiAppSize)
{
    ULONG BytesLoaded;
    CHAR  StatusText[260];
   PVOID Address;
    RtlStringCbPrintfA(StatusText, sizeof(StatusText), "Loading %s", EfiAppName);
    UiDrawStatusText(StatusText);

    EfiAppLoadAddress = MmAllocateMemoryWithType(EfiAppSize, LoaderSystemCode);
    if (EfiAppLoadAddress == NULL)
    {
        TRACE("Failed to allocate 0x%lx bytes for the efi image.\n", EfiAppSize);
        return FALSE;
    }
    Address = EfiAppLoadAddress;
    for (BytesLoaded = 0; BytesLoaded < EfiAppSize; )
    {
        if (ArcRead(FileId, Address, LINUX_READ_CHUNK_SIZE, NULL) != ESUCCESS)
            return FALSE;

        BytesLoaded += LINUX_READ_CHUNK_SIZE;
        Address = (PVOID)((ULONG_PTR)Address + LINUX_READ_CHUNK_SIZE);
      //  UiUpdateProgressBar(BytesLoaded * 100 / (LinuxKernelSize + LinuxInitrdSize), NULL);
    }

    return TRUE;
}

ARC_STATUS
LoadAndBootEFI(
    IN ULONG Argc,
    IN PCHAR Argv[],
    IN PCHAR Envp[])
{
    PCSTR BootPath = NULL;
    PCSTR EfiAppName = NULL;
    FILEINFORMATION FileInfo;
    ULONG EfiFileId = 0;
    UINT32 EfiAppSize;
    TRACE("LoadAndBootEFI: Entry\n");
    ARC_STATUS Status;
    Status = 0;
    /*
     * 1) Grab name of boot file
     * 2) Grab ARC disk number from ini
     * 3) Clock Watchdog
     * 4) Open efi file
     * 5) Adjust EFI_IMAGE
     * 6) Use EFI services to launch efi file
     */
    //TODO Autogenerate:
    //BOOTIA32
    //cdrom(0)
    EfiAppName = "dude";
    /* Fall back to using the system partition as default path */
    BootPath = GetArgumentValue(Argc, Argv, "SystemPartition");
    TRACE("EFiAppName: %s\n", EfiAppName);
    TRACE("BootPath: %s\n", BootPath);

    /* Open the kernel */
    Status = FsOpenFile(EfiAppName, BootPath, OpenReadOnly, &EfiFileId);
    if (Status != ESUCCESS)
    {
        UiMessageBox("EfiFile '%s' not found.", EfiAppName);
        goto EfiBootFailed;
    }

    EfiAppSize = 0;

    Status = ArcGetFileInformation(EfiFileId, &FileInfo);
    if (Status != ESUCCESS || FileInfo.EndingAddress.HighPart != 0)
    {
        EfiAppSize = 0;
    }
    else
    {
        EfiAppSize = (FileInfo.EndingAddress.LowPart - FileInfo.StartingAddress.LowPart) + EFI_PAGE_SIZE * 2;
    }

    BOOLEAN SUCCESS = EfiReadApp(EfiFileId, EfiAppName, EfiAppSize);
    if (SUCCESS == TRUE)
    {

        /* yay */
        TRACE("Loaded efi image\n");
        StartEfiFile((UINTN*)EfiAppLoadAddress, (UINTN)EfiAppSize);
    }
    else{
        TRACE("FAILED\n");
    }

    UiUnInitialize("Chainloading EFI file...");
    IniCleanup();
    for(;;)
    {

    }
    /* Must not return! */
    return ESUCCESS;

EfiBootFailed:

    if (EfiFileId)
        ArcClose(EfiFileId);

    return ENOEXEC;
}
