/*
 * PROJECT:     ReactOS win32 subsystem
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     How user32 and win32k exchange DPI awareness
 */

#pragma once

/* The awareness of a process, a thread or a window, as win32k keeps it */
#define USER_DPI_UNAWARE                0
#define USER_DPI_SYSTEM_AWARE           1
#define USER_DPI_PER_MONITOR_AWARE      2
#define USER_DPI_PER_MONITOR_AWARE_V2   3
#define USER_DPI_UNAWARE_GDISCALED      4
#define USER_DPI_LAST                   USER_DPI_UNAWARE_GDISCALED
#define USER_DPI_INVALID                0xFFFFFFFF  /* a failed query, or a thread without an override */

/* NtUserCallOneParam routines, ours */
#define ONEPARAM_ROUTINE_SETPROCESSDPIAWARENESS 0xfffe000f  /* FALSE with access denied once it was set */
#define ONEPARAM_ROUTINE_GETPROCESSDPIAWARENESS 0xfffe0010  /* a process handle, NULL for the caller */
#define ONEPARAM_ROUTINE_SETTHREADDPIAWARENESS  0xfffe0011  /* USER_DPI_INVALID drops the override; returns the old one */
#define ONEPARAM_ROUTINE_GETTHREADDPIAWARENESS  0xfffe0012  /* the calling thread's; the parameter is unused */

/* NtUserQueryWindow index, ours: the awareness of the thread that created the window */
#define QUERY_WINDOW_DPI_AWARENESS              0x100
#define QUERY_WINDOW_DPI                        0x101   /* the resolution the window lives at */

/* The resolution an application the DWM stretches is shown at */
#define USER_VIRTUAL_DPI                        96

/*
 * The last slot of the TEB's Win32ClientInfo, past CLIENTINFO so gdi32 can read it too:
 * USER_VIRTUAL_DPI while the DWM stretches the thread's windows, else 0.
 */
#define USER_CLIENTINFO_VIRTUAL_DPI_SLOT        61
#define UserGetThreadVirtualDpi() ((UINT)NtCurrentTeb()->Win32ClientInfo[USER_CLIENTINFO_VIRTUAL_DPI_SLOT])

#ifdef SM_CXSCREEN

/* Whether a system metric is a size of something drawn on the screen, so it follows the resolution */
static __inline
BOOL
UserDpiIsSizeMetric(
    _In_ int Index)
{
    switch (Index)
    {
        case SM_CXVSCROLL:
        case SM_CYHSCROLL:
        case SM_CYCAPTION:
        case SM_CXBORDER:
        case SM_CYBORDER:
        case SM_CXDLGFRAME:
        case SM_CYDLGFRAME:
        case SM_CYVTHUMB:
        case SM_CXHTHUMB:
        case SM_CXICON:
        case SM_CYICON:
        case SM_CXCURSOR:
        case SM_CYCURSOR:
        case SM_CYMENU:
        case SM_CYVSCROLL:
        case SM_CXHSCROLL:
        case SM_CXMIN:
        case SM_CYMIN:
        case SM_CXSIZE:
        case SM_CYSIZE:
        case SM_CXFRAME:
        case SM_CYFRAME:
        case SM_CXMINTRACK:
        case SM_CYMINTRACK:
        case SM_CXICONSPACING:
        case SM_CYICONSPACING:
        case SM_CXEDGE:
        case SM_CYEDGE:
        case SM_CXMINSPACING:
        case SM_CYMINSPACING:
        case SM_CXSMICON:
        case SM_CYSMICON:
        case SM_CYSMCAPTION:
        case SM_CXSMSIZE:
        case SM_CYSMSIZE:
        case SM_CXMENUSIZE:
        case SM_CYMENUSIZE:
        case SM_CXMINIMIZED:
        case SM_CYMINIMIZED:
        case SM_CXMENUCHECK:
        case SM_CYMENUCHECK:
#ifdef SM_CXPADDEDBORDER
        case SM_CXPADDEDBORDER:
#endif
        case SM_CXFOCUSBORDER:
        case SM_CYFOCUSBORDER:
            return TRUE;
    }
    return FALSE;
}

/* Whether a system metric is about the screen itself, which looks smaller at 96 DPI too */
static __inline
BOOL
UserDpiIsScreenMetric(
    _In_ int Index)
{
    switch (Index)
    {
        case SM_CXSCREEN:
        case SM_CYSCREEN:
        case SM_CXFULLSCREEN:
        case SM_CYFULLSCREEN:
        case SM_CXMAXTRACK:
        case SM_CYMAXTRACK:
        case SM_CXMAXIMIZED:
        case SM_CYMAXIMIZED:
        case SM_XVIRTUALSCREEN:
        case SM_YVIRTUALSCREEN:
        case SM_CXVIRTUALSCREEN:
        case SM_CYVIRTUALSCREEN:
            return TRUE;
    }
    return FALSE;
}
#endif /* SM_CXSCREEN */
