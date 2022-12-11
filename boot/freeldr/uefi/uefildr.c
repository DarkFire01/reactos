/*
 * PROJECT:     Freeldr UEFI Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI Entry point and helpers
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

#include <uefildr.h>

#include <debug.h>

/* GLOBALS ********************************************************************/

#define TOSTRING_(X) #X
#define TOSTRING(X) TOSTRING_(X)

const PCSTR FrLdrVersionString =
#if (FREELOADER_PATCH_VERSION == 0)
    "FreeLoader v" TOSTRING(FREELOADER_MAJOR_VERSION) "." TOSTRING(FREELOADER_MINOR_VERSION);
#else
    "FreeLoader v" TOSTRING(FREELOADER_MAJOR_VERSION) "." TOSTRING(FREELOADER_MINOR_VERSION) "." TOSTRING(FREELOADER_PATCH_VERSION);
#endif

CCHAR FrLdrBootPath[MAX_PATH] = "";
ULONG_PTR stackSpace;
extern void _changestack();
/* FUNCTIONS ******************************************************************/

EFI_STATUS
EfiEntry(
    _In_ EFI_HANDLE ImageHandle,
    _In_ EFI_SYSTEM_TABLE *SystemTable)
{

    /* Debugger pre-initialization */
    DebugInit(0);

    if(UefiMachInit(ImageHandle, SystemTable) != EFI_SUCCESS)
    {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Failed to start GOP");
        goto Quit;
    }
    SystemTable->BootServices->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, (EFI_PHYSICAL_ADDRESS*)stackSpace);


 //   _changestack();
    if (!UiInitialize(FALSE))
    {
        UiMessageBoxCritical("Unable to initialize UI.");
        goto Quit;
    }

    UefiVideoPutChar('m', 0xFF, 0,0);

    /* Initialize memory manager */
    if (!MmInitializeMemoryManager())
    {
        UiMessageBoxCritical("Unable to initialize memory manager.");
        goto Quit;
    }
    UefiVideoPutChar('C', 0xFF, 32, 32);

    FsInit();
    RunLoader();
Quit:
    for(;;)
    {

    } 
    return 0;
}



// We need to emulate these, because the original ones don't work in freeldr
// These functions are here, because they need to be in the main compilation unit
// and cannot be in a library.
int __cdecl wctomb(char *mbchar, wchar_t wchar)
{
    *mbchar = (char)wchar;
    return 1;
}

int __cdecl mbtowc(wchar_t *wchar, const char *mbchar, size_t count)
{
    *wchar = (wchar_t)*mbchar;
    return 1;
}

// The wctype table is 144 KB, too much for poor freeldr
int __cdecl iswctype(wint_t wc, wctype_t wctypeFlags)
{
    return _isctype((char)wc, wctypeFlags);
}
