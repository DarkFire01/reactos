#include "include/rosefip.h"

/* 
 * This function is really just to display the issue then shutdown the computer. 
 * but its also a meme 
 */

VOID
RefiFatalFailure(_In_ EFI_SYSTEM_TABLE *SystemTable, 
                 _In_ EFI_STATUS refiCheck,
                 _In_ EFI_GRAPHICS_OUTPUT_PROTOCOL* gop, _In_ UINT32 x, UINT32 y)
{
    RefiClearScreen(SystemTable);
    RefiSetColor(SystemTable, EFI_BLUE);
    CreateFilledBox(0, 0, x, y, SetGraphicsColor(BLUE), gop);
    RefiSetColor(SystemTable, EFI_WHITE);
    RefiColPrint(SystemTable, L"A problem has been detected and ROSEFI has been shut down to prevent damage     to your computer.\r\n");
    RefiColPrint(SystemTable, L"\r\nROSEFI_ROCKS\r\n");
    RefiColPrint(SystemTable, L"\r\nIf this is the first time you've seen this Stop error screen,\r\n");
    RefiColPrint(SystemTable, L"restart your computer. If this screen appears again, follow\r\n");
    RefiColPrint(SystemTable, L"these steps:\r\n");
    RefiColPrint(SystemTable, L"\r\nCheck to make sure any new hardware or software is proper installed,\r\n");
    RefiColPrint(SystemTable, L"If this is a new installation, ask your hardware or software manufacturer\r\n");
    RefiColPrint(SystemTable, L"For any ROSEFI updates you might need.\r\n");
    RefiColPrint(SystemTable, L"\r\nIf problems continue, disable or remove any newly installed hardware\r\n");
    RefiColPrint(SystemTable, L"or software. Disable BIOS memory options such as caching or shadowing.\r\n");
    RefiColPrint(SystemTable, L"If you need to use Safe Mode to remove or disable components, restart\r\n");
    RefiColPrint(SystemTable, L"your computer, press F8 to select Advanced Startup Options, and then\r\n");
    RefiColPrint(SystemTable, L"select Safe Mode.\r\n");

    RefiColPrint(SystemTable, L"\r\nTechnical information:\r\n");
    RefiColPrint(SystemTable, L"\r\n*** STOP: 0xDEADBEEF (You're fucked)\r\n");


    RefiColPrint(SystemTable, L"\r\nRebooting in a few seconds...\r\n");
    RefiStallProcessor(SystemTable, 5000);
    SystemTable->RuntimeServices->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, 0);
}