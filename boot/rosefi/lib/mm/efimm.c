#include "mm.h"

EFI_STATUS
RefiInitMemoryManager(_In_ EFI_SYSTEM_TABLE *SystemTable)
{
    RefiSetColor(SystemTable, EFI_GREEN);
    RefiColPrint(SystemTable, L"RefiInitMemoryManager: ");
    RefiSetColor(SystemTable, EFI_WHITE);
    RefiColPrint(SystemTable, L"Initalizing Memory Manager\r\n");

    /* Get UEFI memory map */

    return EFI_SUCCESS;
}