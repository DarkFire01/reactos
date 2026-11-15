/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     Messages between shell components over the message session
 */

#pragma once

#include <windows.h>

#define SHELL_ENDPOINT_CHROME L"System\\AppChromeServerEndpoint"
#define SHELL_ENDPOINT_HOST   L"System\\ShellHostEndpoint"

#define SHELL_TEXT_MAX 48

typedef enum _SHELL_MSG_ID
{
    /* To the chrome server */
    ShellMsgSetTitle = 1,
    ShellMsgSetSoftKeys,
    ShellMsgSetCloseButton,
    ShellMsgSetMode,
    ShellMsgSetNotify,

    /* To the shell host (MobileUI) */
    ShellMsgStartInvoked = 0x100,
    ShellMsgSoftKeyInvoked,
    ShellMsgCloseInvoked
} SHELL_MSG_ID;

typedef enum _SHELL_CLOSE_KIND
{
    ShellCloseNone = 0,
    ShellCloseOk,
    ShellCloseX
} SHELL_CLOSE_KIND;

typedef enum _SHELL_MODE
{
    ShellModeHome = 0,
    ShellModeApp
} SHELL_MODE;

typedef enum _SHELL_SOFTKEY
{
    ShellSoftKeyLeft = 0,
    ShellSoftKeyRight,
    ShellSoftKeyCenter
} SHELL_SOFTKEY;

typedef struct _SHELL_MESSAGE
{
    SHELL_MSG_ID Id;
    ULONG Value;
    WCHAR Text[2][SHELL_TEXT_MAX];
} SHELL_MESSAGE, *PSHELL_MESSAGE;
