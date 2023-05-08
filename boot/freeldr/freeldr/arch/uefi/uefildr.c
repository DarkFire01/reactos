/*
 * PROJECT:     FreeLoader UEFI Support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Entry point and helpers
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

#include <uefildr.h>

#include "../../../../environ/include/bl.h"
#include <debug.h>

/* GLOBALS ********************************************************************/

EFI_HANDLE GlobalImageHandle;
EFI_SYSTEM_TABLE *GlobalSystemTable;

/* FUNCTIONS ******************************************************************/

/*
 *
 * Both of these parameters to EFI_ENTRY are pointers.
 * awkwardly, so is BlAppicationEntry.
 * So behold the birth of the weirdest fucking idea Illen has ever given me
 * Freeldr.epic
 */
EFI_STATUS
EfiEntry(
    _In_ PVOID ImageHandle,
    _In_ PVOID SystemTable)
{
    PBOOT_APPLICATION_PARAMETER_BLOCK BootAppParameters;
	PBL_LIBRARY_PARAMETERS LibraryParameters;
    PBL_LIBRARY_PARAMETERS	LibraryParams;
	PBL_FIRMWARE_DESCRIPTOR FirmwareDescriptor;

    /* This is a cheap trick... here's the TLDR: */
    EFI_SYSTEM_TABLE *TestSystemTable;
    UINT64 Signature = EFI_SYSTEM_TABLE_SIGNATURE; /* Grab the STATIC SYSTEMTABLE SIGNATURE */
    TestSystemTable = (EFI_SYSTEM_TABLE*)SystemTable;
    if (TestSystemTable->Hdr.Signature == Signature)
    {
            /* oh my god */
        GlobalImageHandle = (EFI_HANDLE)ImageHandle;
        GlobalSystemTable = (EFI_SYSTEM_TABLE*)SystemTable;
        /* we are on a UEFI natural entry point*/
    }
    else
    {
        /* jesus fucking christ*/
        BootAppParameters = (PBOOT_APPLICATION_PARAMETER_BLOCK)ImageHandle;
	    LibraryParameters = (PBL_LIBRARY_PARAMETERS)SystemTable;

        UINTN ParamPointer;

	    if (!BootAppParameters)
	    {
	    	// Invalid parameter
	    	return STATUS_INVALID_PARAMETER;
	    }

	    LibraryParams = LibraryParameters;
	    ParamPointer = (UINTN) BootAppParameters;
	    FirmwareDescriptor = (PBL_FIRMWARE_DESCRIPTOR) (ParamPointer + BootAppParameters->FirmwareParametersOffset);

	    // Switch mode
	  //  SwitchToRealModeContext(FirmwareDescriptor);

	    // Do what ever you want now
	    if (FirmwareDescriptor->SystemTable)
	    {
            GlobalImageHandle = FirmwareDescriptor->ImageHandle;
			GlobalSystemTable = FirmwareDescriptor->SystemTable;
	    }
        /* Some idiot decided to boot freeldr as a bootmgr app*/
    }

    GlobalSystemTable->ConOut->OutputString(GlobalSystemTable->ConOut,L"Starting Freeldr from bootmgr\n");
    for(;;)
    {

    }
    BootMain(NULL);

    UNREACHABLE;
    return 0;
}

#ifndef _M_ARM
VOID __cdecl Reboot(VOID)
{

}
#endif
