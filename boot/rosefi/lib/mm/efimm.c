#include "mm.h"

EFI_STATUS
RefiInitMemoryManager(_In_ EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_STATUS refiCheck;

    RefiColPrint(SystemTable, L"RefiInitMemoryManager: ");
    RefiColPrint(SystemTable, L"Initalizing Memory Manager\r\n");
    refiCheck = EFI_ACCESS_DENIED;
    /* Get UEFI memory map */

    return refiCheck;
}