/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     Wire format between the shell and out of process apps
 */

#pragma once

#include <windows.h>

/*
 * One duplex message mode pipe per app. The app renders into a shared texture and
 * the shell composes it. Nothing else crosses the boundary, so the transport can
 * later move to a sandbox (container or VM) without changing apps.
 */

#define APP_TEXT_MAX 64

typedef enum _APP_MSG_ID
{
    /* Shell to app */
    AppMsgActivate = 1,
    AppMsgPointer,
    AppMsgKey,
    AppMsgSoftKey,
    AppMsgCloseButton,
    AppMsgTheme,
    AppMsgTerminate,
    AppMsgChar,
    AppMsgWheel,
    AppMsgMenuPick,
    AppMsgOpenDocument,

    /* App to shell */
    AppMsgReady = 0x100,
    AppMsgPresented,
    AppMsgChrome,
    AppMsgRequestClose,
    AppMsgRequestHide,
    AppMsgMenu,
    AppMsgNotify,
    AppMsgInputPanel,
    AppMsgLaunch,
    AppMsgRequest
} APP_MSG_ID;

#define APP_READY_KEYED_MUTEX 0x1

/* AppMsgMenu anchors */
typedef enum _APP_MENU_ANCHOR
{
    AppMenuLeftSoftKey = 0,
    AppMenuRightSoftKey,
    AppMenuPoint
} APP_MENU_ANCHOR;

/* AppMsgMenu: Path holds items separated by '\n', a lone "-" is a separator */
#define APP_MENU_SEPARATOR L"-"

typedef struct _APP_MESSAGE
{
    ULONG Id;
    ULONG Flags;

    /*
     * Activate: Int[0..1] pixel size, Float[0] scale, Float[1..2] logical size.
     * Pointer: Int[0] UIX_POINTER, Float[0..1] logical position.
     * Key: Int[0] virtual key. SoftKey: Int[0] SHELL_SOFTKEY.
     * Ready: Handle, Int[0..1] pixel size. Chrome: Int[0] SHELL_CLOSE_KIND.
     * Char: Int[0] code point. Wheel: Float[0..1] position, Float[2] notches.
     * MenuPick: Int[0] item index, -1 when dismissed.
     * Menu: Int[0] APP_MENU_ANCHOR, Float[0..1] point. Notify: Text[0] title, Path body.
     * InputPanel: Int[0] nonzero to show. Launch: Text[0] app id, Path an optional document.
     * OpenDocument: Path, sent to a running app that was launched with a document.
     * Request: Text[0] a shell command from the short list the shell allows apps.
     */
    LONG Int[4];
    FLOAT Float[4];
    ULONGLONG Handle;

    /* Chrome: title, left, right. Theme: Path is the theme file */
    WCHAR Text[3][APP_TEXT_MAX];
    WCHAR Path[MAX_PATH];
} APP_MESSAGE, *PAPP_MESSAGE;
