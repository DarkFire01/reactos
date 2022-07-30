#include <uefildr.h>


#include <debug.h>
DBG_DEFAULT_CHANNEL(WARNING);

/* GLOBALS ********************************************************************/

#define TOSTRING_(X) #X
#define TOSTRING(X) TOSTRING_(X)

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
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop;
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"UEFI EntryPoint: Starting freeldr from UEFI");

    UefiMachInit(0);

   // FrLdrCheckCpuCompatibility();

    UefiInitConsole(SystemTable);
    SystemTable->BootServices->LocateProtocol(&EfiGraphicsOutputProtocol, 0, (void**)&gop);
    UefiInitalizeVideo(ImageHandle, SystemTable, gop);

    UefiVideoClearScreen(0);
    UefiPrintF("Graphics initalization Complete", 1, 1, 0xFFFFFF, 0x000000);
for(;;)
{
    
}
    if (!UiInitialize(FALSE))
    {
        UiMessageBoxCritical("Unable to initialize UI.");
        goto Quit;
    }

#if 0
    /* Initialize memory manager */
    if (!MmInitializeMemoryManager())
    {
        UiMessageBoxCritical("Unable to initialize memory manager.");
        goto Quit;
    }
#endif


    UefiInitializeFileSystemSupport(ImageHandle, SystemTable);
    FsInit();

    if (!MachInitializeBootDevices())
    {
        UiMessageBoxCritical("Error when detecting hardware.");
    }

    RunLoader();

    Quit:
    /* If we reach this point, something went wrong before, therefore reboot */
        return EFI_SUCCESS;
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

#ifdef _M_ARM

UCHAR MachDefaultTextColor = COLOR_GRAY;

static unsigned int delay_count = 1;

static
VOID
__StallExecutionProcessor(ULONG Loops)
{
    register volatile unsigned int i;
    for (i = 0; i < Loops; i++);
}

VOID StallExecutionProcessor(ULONG Microseconds)
{
    ULONGLONG LoopCount = ((ULONGLONG)delay_count * (ULONGLONG)Microseconds) / 1000ULL;
    __StallExecutionProcessor((ULONG)LoopCount);
}

VOID
HalpCalibrateStallExecution(VOID)
{
}

#endif

#ifdef _M_ARM64


VOID
FrLdrBugCheckWithMessage(
    ULONG BugCode,
    PCHAR File,
    ULONG Line,
    PSTR Format,
    ...)
{

}


VOID
DbgBreakPoint(VOID)
{

}
VOID
HalpCalibrateStallExecution(VOID)
{
}

#endif