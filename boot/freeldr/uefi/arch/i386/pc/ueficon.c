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