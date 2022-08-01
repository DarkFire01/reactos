#include <uefildr.h>


#include <debug.h>
DBG_DEFAULT_CHANNEL(WARNING);

EFI_SYSTEM_TABLE* LocSystemTable;
EFI_HANDLE LocImageHandle;

VOID
UefiInitializeInputSupport(_In_ EFI_HANDLE ImageHandle,
                                _In_ EFI_SYSTEM_TABLE *SystemTable)
{
    LocSystemTable = SystemTable;
    LocImageHandle = ImageHandle;
}

VOID
UefiConsoleReset()
{
   LocSystemTable->ConIn->Reset(LocSystemTable->ConIn, FALSE);
}

VOID
UefiWaitForAnyKey()
{
    EFI_STATUS Status;
    EFI_INPUT_KEY Key;
    while ((Status = LocSystemTable->ConIn->ReadKeyStroke(LocSystemTable->ConIn,&Key)) == EFI_NOT_READY);
}

VOID
UefiPollAndDrawKeyboardInput()
{
    EFI_INPUT_KEY Key;
    UINT32 count;
    count = 0;
    for(;;)
    {
      LocSystemTable->ConIn->ReadKeyStroke(LocSystemTable->ConIn, &Key);
      if(Key.UnicodeChar != 0)
      {
        UefiVideoOutputChar(Key.UnicodeChar, 0 + count, 0, 0xFFFFFF, 0x000000);
        count += 1;
        LocSystemTable->BootServices->Stall(1000);
      }
    }
}
