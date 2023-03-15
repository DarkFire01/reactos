/*
 * PROJECT:     Freeldr UEFI Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI Entry point and helpers
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

#include <uefildr.h>

#include <debug.h>
DBG_DEFAULT_CHANNEL(WARNING);
/* GLOBALS ********************************************************************/

EFI_HANDLE GlobalImageHandle;
EFI_SYSTEM_TABLE *GlobalSystemTable;

/* FUNCTIONS ******************************************************************/
extern ULONG_PTR PINEUARTBASE;
EFI_STATUS
EfiEntry(
    _In_ EFI_HANDLE ImageHandle,
    _In_ EFI_SYSTEM_TABLE *SystemTable)
{
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"UEFI EntryPoint: Starting freeldr from UEFI");
    GlobalImageHandle = ImageHandle;
    GlobalSystemTable = SystemTable;
    DebugInit(0);
    MachInit(NULL);

        /* UI pre-initialization */
    if (!UiInitialize(FALSE))
    {
        UiMessageBoxCritical("Unable to initialize UI.");

    }


    /* Initialize memory manager */
    if (!MmInitializeMemoryManager())
    {
        UiMessageBoxCritical("Unable to initialize memory manager.");
        //goto Quit;
    }
    printf("Setting up boot now...\n");
      FsInit();

    RunLoader();

   // TRACE("okay that worked.. trying to setup mach now\n");
    for(;;)
    {

    }
    BootMain(NULL);

    //UNREACHABLE
    return 0;
}


VOID __cdecl Reboot(VOID)
{

}
