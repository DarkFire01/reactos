#include <uefildr.h>

#include <debug.h>

EFI_SYSTEM_TABLE *LocSystemTable;

VOID
UefiInitConsole(_In_ EFI_SYSTEM_TABLE *SystemTable)
{
    LocSystemTable = SystemTable;
}

VOID
UefiConsPutChar(int Ch)
{
    LocSystemTable->ConOut->OutputString(LocSystemTable->ConOut, L"h");
}

BOOLEAN
UefiConsKbHit(VOID)
{
    /* No keyboard support yet */
    return FALSE;
}

int
UefiConsGetCh(void)
{
    /* No keyboard support yet */
    while (1) ;

    return 0;
}
