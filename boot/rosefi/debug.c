#include "include/rosefip.h"

unsigned short int* CheckStandardEFIError(unsigned long long s)
{
    switch(s)
    {
        case EFI_LOAD_ERROR:
        {
            return (unsigned short int*)L" Load Error\r\n";
        }
        case EFI_INVALID_PARAMETER:
        {
            return (unsigned short int*)L" Invalid Parameter\r\n";
        }
        case EFI_UNSUPPORTED:
        {
            return (unsigned short int*)L" Unsupported\r\n";
        }
        case EFI_BAD_BUFFER_SIZE:
        {
            return (unsigned short int*)L" Bad Buffer Size\r\n";
        }
        case EFI_BUFFER_TOO_SMALL:
        {
            return (unsigned short int*)L" Buffer Too Small\r\n";
        }
        case EFI_NOT_READY:
        {
            return (unsigned short int*)L" Not Ready\r\n";
        }
        case EFI_DEVICE_ERROR:
        {
            return (unsigned short int*)L" Device Error\r\n";
        }
        case EFI_WRITE_PROTECTED:
        {
            return (unsigned short int*)L" Write Protected\r\n";
        }
        case EFI_OUT_OF_RESOURCES:
        {
            return (unsigned short int*)L" Out Of Resources\r\n";
        }
        case EFI_VOLUME_CORRUPTED:
        {
            return (unsigned short int*)L" Volume Corrupted\r\n";
        }
        case EFI_VOLUME_FULL:
        {
            return (unsigned short int*)L" Volume Full\r\n";
        }
        case EFI_NO_MEDIA:
        {
            return (unsigned short int*)L" No Media\r\n";
        }
        case EFI_MEDIA_CHANGED:
        {
            return (unsigned short int*)L" Media Changed\r\n";
        }
        case EFI_NOT_FOUND:
        {
            return (unsigned short int*)L" File Not Found\r\n";
        }
        case EFI_ACCESS_DENIED:
        {
            return (unsigned short int*)L" Access Denied\r\n";
        }
        case EFI_NO_RESPONSE:
        {
            return (unsigned short int*)L" No Response\r\n";
        }
        case EFI_NO_MAPPING:
        {
            return (unsigned short int*)L" No Mapping\r\n";
        }
        case EFI_TIMEOUT:
        {
            return (unsigned short int*)L" Timeout\r\n";
        }
        case EFI_NOT_STARTED:
        {
            return (unsigned short int*)L" Not Started\r\n";
        }
        case EFI_ALREADY_STARTED:
        {
            return (unsigned short int*)L" Already Started\r\n";
        }
        case EFI_ABORTED:
        {
            return (unsigned short int*)L" Aborted\r\n";
        }
        case EFI_ICMP_ERROR:
        {
            return (unsigned short int*)L" ICMP Error\r\n";
        }
        case EFI_TFTP_ERROR:
        {
            return (unsigned short int*)L" TFTP Error\r\n";
        }
        case EFI_PROTOCOL_ERROR:
        {
            return (unsigned short int*)L" Protocol Error\r\n";
        }
        case EFI_INCOMPATIBLE_VERSION:
        {
            return (unsigned short int*)L" Incompatible Version\r\n";
        }
        case EFI_SECURITY_VIOLATION:
        {
            return (unsigned short int*)L" Security Violation\r\n";
        }
        case EFI_CRC_ERROR:
        {
            return (unsigned short int*)L" CRC Error\r\n";
        }
        case EFI_END_OF_MEDIA:
        {
            return (unsigned short int*)L" End Of Media\r\n";
        }
        case EFI_END_OF_FILE:
        {
            return (unsigned short int*)L" End Of File\r\n";
        }
        case EFI_INVALID_LANGUAGE:
        {
            return (unsigned short int*)L" Invalid Language\r\n";
        }
        case EFI_COMPROMISED_DATA:
        {
            return (unsigned short int*)L" Compromised Data\r\n";
        }
        case EFI_IP_ADDRESS_CONFLICT:
        {
            return (unsigned short int*)L" IP Address Conflict\r\n";
        }
        case EFI_HTTP_ERROR:
        {
            return (unsigned short int*)L" End Of File\r\n";
        }
        case EFI_WARN_UNKNOWN_GLYPH:
        {
            return (unsigned short int*)L" WARNING - Unknown Glyph\r\n";
        }
        case EFI_WARN_DELETE_FAILURE:
        {
            return (unsigned short int*)L" WARNING - Delete Failure\r\n";
        }
        case EFI_WARN_WRITE_FAILURE:
        {
            return (unsigned short int*)L" WARNING - Write Failure\r\n";
        }
        case EFI_WARN_BUFFER_TOO_SMALL:
        {
            return (unsigned short int*)L" WARNING - Buffer Too Small\r\n";
        }
        case EFI_WARN_STALE_DATA:
        {
            return (unsigned short int*)L" WARNING - Stale Data\r\n";
        }
        case EFI_WARN_FILE_SYSTEM:
        {
            return (unsigned short int*)L" WARNING - File System\r\n";
        }
        case EFI_WARN_RESET_REQUIRED:
        {
            return (unsigned short int*)L" WARNING - Reset Required\r\n";
        }
        case EFI_SUCCESS:
        {
            return (unsigned short int*)L" Successful\r\n";
        }
    }
    return (unsigned short int*)L" ERROR\r\n";
}

/* These boot service macros are useful */

VOID
RefiColSetCursor(EFI_SYSTEM_TABLE* SystemTable, UINT32 Col, UINT32 Row)
{
    SystemTable->ConOut->SetCursorPosition(SystemTable->ConOut, Col, Row);
}

VOID
RefiColPrint(EFI_SYSTEM_TABLE* SystemTable, CHAR16* str)
{
    SystemTable->ConOut->OutputString(SystemTable->ConOut, str);
}

VOID
RefiColClearScreen(EFI_SYSTEM_TABLE* SystemTable)
{
    SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
}

VOID
RefiColSetColor(EFI_SYSTEM_TABLE* SystemTable, UINTN Attribute)
{
    SystemTable->ConOut->SetAttribute(SystemTable->ConOut, Attribute);
}

/* 
 * Fuck Virtualbox
 */

VOID
RefiTrollBSoD(_In_ EFI_SYSTEM_TABLE *SystemTable)
{
    RefiClearScreen(0x0000FF);
    RefiColSetColor(SystemTable, EFI_WHITE);
    RefiColPrint(SystemTable, L"A problem has been detected and ROSEFI has been shut down to prevent damage     to your computer.\r\n");
    RefiColPrint(SystemTable, L"\r\nVIRTUALBOX_SUCKS\r\n");
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
    RefiColPrint(SystemTable, L"\r\n*** STOP: 0xDEADBEEF (You're using virtualbox aren't you)\r\n");


    RefiColPrint(SystemTable, L"\r\nRebooting in a few seconds...\r\n");
    RefiStallProcessor(SystemTable, 10000);
    SystemTable->RuntimeServices->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, 0);
}

