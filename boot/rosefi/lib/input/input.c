
VOID
RefiHitAnyKey(EFI_SYSTEM_TABLE* SystemTable)
{
}

VOID
RefiResetKeyboard(EFI_SYSTEM_TABLE* SystemTable)
{
    SystemTable->ConIn->Reset(SystemTable->ConIn, 1);
}

BOOLEAN 
RefiGetKey(CHAR16 key, EFI_INPUT_KEY CheckKeystroke)
{
    if(CheckKeystroke.UnicodeChar == key)
    {
        return 1;
    } 
    else 
    {
        return 0;
    }
}

EFI_STATUS 
RefiCheckKey(EFI_SYSTEM_TABLE* SystemTable, EFI_INPUT_KEY CheckKeystroke)
{
    return SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &CheckKeystroke);
}

