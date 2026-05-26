/*
 * PROJECT:     Xbox NV2A OpenGL 1.x ICD
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     DLL entry point
 */

#include "xboxogl.h"

BOOL WINAPI
DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reserved;
    switch (reason)
    {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(inst);
            break;
        case DLL_PROCESS_DETACH:
            break;
    }
    return TRUE;
}
