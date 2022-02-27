

VOID
RefiHitAnyKey(EFI_SYSTEM_TABLE* SystemTable);

VOID
RefiResetKeyboard(EFI_SYSTEM_TABLE* SystemTable);

BOOLEAN 
RefiGetKey(CHAR16 key, EFI_INPUT_KEY CheckKeystroke);

EFI_STATUS 
RefiCheckKey(EFI_SYSTEM_TABLE* SystemTable, EFI_INPUT_KEY CheckKeystroke);


VOID
RefiUefiHitAnyKey(EFI_SYSTEM_TABLE* SystemTable);

VOID
RefiUefiResetKeyboard(EFI_SYSTEM_TABLE* SystemTable);

BOOLEAN 
RefiUefiGetKey(CHAR16 key, EFI_INPUT_KEY CheckKeystroke);

EFI_STATUS 
RefiUefiCheckKey(EFI_SYSTEM_TABLE* SystemTable, EFI_INPUT_KEY CheckKeystroke);
