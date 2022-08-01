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
    LocSystemTable->ConOut->OutputString(LocSystemTable->ConOut, (CHAR16*)&Ch);
}

BOOLEAN
UefiConsKbHit(VOID)
{
    EFI_INPUT_KEY Key;
    LocSystemTable->ConIn->ReadKeyStroke(LocSystemTable->ConIn, &Key);
    if(Key.UnicodeChar != 0)
    {
        return TRUE;
    }
    else
    {
        LocSystemTable->ConIn->Reset(LocSystemTable->ConIn, FALSE);
    }

    return FALSE;
}

int
UefiConsGetCh(void)
{
    EFI_INPUT_KEY Key;
    LocSystemTable->ConIn->ReadKeyStroke(LocSystemTable->ConIn, &Key);
    int keystroke = Key.UnicodeChar;
    LocSystemTable->ConIn->Reset(LocSystemTable->ConIn, FALSE);
    return keystroke;
}

VOID
UefiConsSetCursor(UINT32 Col, UINT32 Row)
{
    LocSystemTable->ConOut->SetCursorPosition(LocSystemTable->ConOut, Col, Row);
}

