#include <Windows.h>
#include <stdio.h>

static HWND (WINAPI *pCreateWindowExW)(ULONG, LPCWSTR className, LPCWSTR windowName, ULONG windowStyle, int, int, int, int, PVOID, PVOID, HINSTANCE, PVOID) = NULL;
static ATOM (WINAPI *pRegisterClassW)(WNDCLASSW* pClass) = NULL;
static LRESULT (CALLBACK *pDefWindowProcW)(HWND, UINT, WPARAM, LPARAM);
static BOOL (WINAPI *pShowWindow)(HWND, int);
static BOOL (WINAPI *pUpdateWindow)(HWND);
static BOOL (WINAPI *pGetMessageW)(MSG*, HWND, UINT, UINT);
static BOOL (WINAPI *pDispatchMessageW)(MSG*);
static HDC (WINAPI *pBeginPaint)(HWND, PAINTSTRUCT*);
static void (WINAPI *pFillRect)(HDC, RECT*, HBRUSH);
static void (WINAPI *pEndPaint)(HWND, PAINTSTRUCT*);

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    return pDefWindowProcW(hWnd, msg, wParam, lParam);
}

void DelayLoadUser32()
{
    HMODULE hUser32 = LoadLibraryW(L"user32.dll");
    
    pCreateWindowExW = (PVOID)GetProcAddress(hUser32, "CreateWindowExW");
    pRegisterClassW  = (PVOID)GetProcAddress(hUser32, "RegisterClassW");
    pDefWindowProcW  = (PVOID)GetProcAddress(hUser32, "DefWindowProcW");

    pUpdateWindow     = (PVOID)GetProcAddress(hUser32, "UpdateWindow");
    pShowWindow       = (PVOID)GetProcAddress(hUser32, "ShowWindow");
    pGetMessageW      = (PVOID)GetProcAddress(hUser32, "GetMessageW");
    pDispatchMessageW = (PVOID)GetProcAddress(hUser32, "DispatchMessageW");
    
    pBeginPaint  = (PVOID)GetProcAddress(hUser32, "BeginPaint");
    pEndPaint    = (PVOID)GetProcAddress(hUser32, "EndPaint");
    pFillRect    = (PVOID)GetProcAddress(hUser32, "FillRect");
}

void Print(const wchar_t* wszBuffer)
{
    size_t i = 0;
    while(wszBuffer[i++]);
    
    WriteConsole(GetStdHandle(STD_OUTPUT_HANDLE), wszBuffer, i, NULL, NULL);
}

int wmainCRTStartup()
{
    WCHAR wszHello[] = L"Hello World!\n";
    
    AllocConsole();
    
    SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);
    
    __debugbreak();
    Print(wszHello);
    
    DelayLoadUser32();
    Print(L"Hello2\n");
    
    WNDCLASSW wndClass = { 0 };
    wndClass.hInstance = (HINSTANCE)GetModuleHandle(NULL);
    wndClass.lpszClassName = wszHello;
    wndClass.lpfnWndProc = WndProc;
    wndClass.style = CS_HREDRAW | CS_VREDRAW;
    wndClass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    
    pRegisterClassW(&wndClass);
    
    HWND result = pCreateWindowExW(0, wszHello, wszHello, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, wndClass.hInstance, NULL);
    
    Print(L"Hello2.5\n");
    
    if (result == 0 || result == INVALID_HANDLE_VALUE)
    {
        return -1;
    }
    
    pUpdateWindow(result);
    pShowWindow(result, 1);
    
    MSG msg;
    
    Print(L"Hello3\n");
    
    while(pGetMessageW(&msg, 0, 0, 0))
    {
        pDispatchMessageW(&msg);
    }
    
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}