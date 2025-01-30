#include <Windows.h>

int wmainCRTStartup()
{
    WCHAR wszHello[] = L"Hello World!\n";
    
    AllocConsole();
    
    WriteConsole(GetStdHandle(STD_OUTPUT_HANDLE), wszHello, sizeof(wszHello) / sizeof(*wszHello) - 1, NULL, NULL);
    
    while (1);
    
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}