#include <uefildr.h>


#include <debug.h>

/* GLOBALS ********************************************************************/

#define TOSTRING_(X) #X
#define TOSTRING(X) TOSTRING_(X)
#define UefiBoot FALSE
EFI_GUID EfiGraphicsOutputProtocol = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
const PCSTR FrLdrVersionString =
#if (FREELOADER_PATCH_VERSION == 0)
    "FreeLoader v" TOSTRING(FREELOADER_MAJOR_VERSION) "." TOSTRING(FREELOADER_MINOR_VERSION);
#else
    "FreeLoader v" TOSTRING(FREELOADER_MAJOR_VERSION) "." TOSTRING(FREELOADER_MINOR_VERSION) "." TOSTRING(FREELOADER_PATCH_VERSION);
#endif

CCHAR FrLdrBootPath[MAX_PATH] = "";

EFI_STATUS
EfiEntry(
    _In_ EFI_HANDLE ImageHandle,
    _In_ EFI_SYSTEM_TABLE *SystemTable)
{
    MachInit(0);

    FrLdrCheckCpuCompatibility();

    UefiInitConsole(SystemTable);

    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop;
    SystemTable->BootServices->LocateProtocol(&EfiGraphicsOutputProtocol, 0, (void**)&gop);
    UefiInitalizeVideo(ImageHandle, SystemTable, gop);


    /* Initialize I/O subsystem */
    FsInit();
    RunLoader();
Quit:
    /* If we reach this point, something went wrong before, therefore freeze */
    for(;;)
    {

    }
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


VOID __cdecl BootMain(IN PCCH CmdLine)
{
    NOTHING;
}
