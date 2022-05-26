#include "precomp.h"

int WINAPI main(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPTSTR lpCmdLine, int nShowCmd)
{
MSG msg;
WNDCLASSW wc;
HDC hDC;

 wc.hbrBackground = (HBRUSH)COLOR_WINDOW;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hInstance = hInstance;
    wc.lpszClassName = L"RingBullet";

    HWND hWnd = CreateWindowW(L"RingBullet", L"RingBullet-Engine", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 
        100, 100, 1920, 1080,
        NULL, NULL, NULL, NULL);
        
    if (hWnd == NULL) {
        return -1;
    }
   // ShowWindow(hWnd, nCmdShow);
    hDC = GetDC(hWnd);
    
    while(GetMessageW(&msg, NULL, 0,0))
    {
        
        
    }
    
    
    return 0;
}