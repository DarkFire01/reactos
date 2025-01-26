/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS Console Server DLL
 * FILE:            include/reactos/subsys/win/conmsg.h
 * PURPOSE:         Public definitions for communication
 *                  between Console API Clients and Servers
 * PROGRAMMERS:     Hermes Belusca-Maito (hermes.belusca@sfr.fr)
 */

#ifndef _CONMSG_H
#define _CONMSG_H

#pragma once

#define CONSRV_SERVERDLL_INDEX      2
#define CONSRV_FIRST_API_NUMBER     512

// Windows Server 2003 table from http://j00ru.vexillium.org/csrss_list/api_list.html#Windows_2k3
typedef enum _CONSRV_API_NUMBER
{
    ConsolepOpenConsole = CONSRV_FIRST_API_NUMBER,
    ConsolepGetConsoleInput,
    ConsolepWriteConsoleInput,
    ConsolepReadConsoleOutput,
    ConsolepWriteConsoleOutput,
    ConsolepReadConsoleOutputString,
    ConsolepWriteConsoleOutputString,
    ConsolepFillConsoleOutput,
    ConsolepGetMode,
    ConsolepGetNumberOfFonts,
    ConsolepGetNumberOfInputEvents,
    ConsolepGetScreenBufferInfo,
    ConsolepGetCursorInfo,
    ConsolepGetMouseInfo,
    ConsolepGetFontInfo,
    ConsolepGetFontSize,
    ConsolepGetCurrentFont,
    ConsolepSetMode,
    ConsolepSetActiveScreenBuffer,
    ConsolepFlushInputBuffer,
    ConsolepGetLargestWindowSize,
    ConsolepSetScreenBufferSize,
    ConsolepSetCursorPosition,
    ConsolepSetCursorInfo,
    ConsolepSetWindowInfo,
    ConsolepScrollScreenBuffer,
    ConsolepSetTextAttribute,
    ConsolepSetFont,
    ConsolepSetIcon,
    ConsolepReadConsole,
    ConsolepWriteConsole,
    ConsolepDuplicateHandle,
    ConsolepGetHandleInformation,
    ConsolepSetHandleInformation,
    ConsolepCloseHandle,
    ConsolepVerifyIoHandle,
    ConsolepAlloc,                          // Not present in Win7
    ConsolepFree,                           // Not present in Win7
    ConsolepGetTitle,
    ConsolepSetTitle,
    ConsolepCreateScreenBuffer,
    ConsolepInvalidateBitMapRect,
    ConsolepVDMOperation,
    ConsolepSetCursor,
    ConsolepShowCursor,
    ConsolepMenuControl,
    ConsolepSetPalette,
    ConsolepSetDisplayMode,
    ConsolepRegisterVDM,
    ConsolepGetHardwareState,
    ConsolepSetHardwareState,
    ConsolepGetDisplayMode,
    ConsolepAddAlias,
    ConsolepGetAlias,
    ConsolepGetAliasesLength,
    ConsolepGetAliasExesLength,
    ConsolepGetAliases,
    ConsolepGetAliasExes,
    ConsolepExpungeCommandHistory,
    ConsolepSetNumberOfCommands,
    ConsolepGetCommandHistoryLength,
    ConsolepGetCommandHistory,
    ConsolepSetCommandHistoryMode,          // Not present in Vista+
    ConsolepGetCP,
    ConsolepSetCP,
    ConsolepSetKeyShortcuts,
    ConsolepSetMenuClose,
    ConsolepNotifyLastClose,
    ConsolepGenerateCtrlEvent,
    ConsolepGetKeyboardLayoutName,
    ConsolepGetConsoleWindow,
    ConsolepCharType,
    ConsolepSetLocalEUDC,
    ConsolepSetCursorMode,
    ConsolepGetCursorMode,
    ConsolepRegisterOS2,
    ConsolepSetOS2OemFormat,
    ConsolepGetNlsMode,
    ConsolepSetNlsMode,
    ConsolepRegisterConsoleIME,             // Not present in Win7
    ConsolepUnregisterConsoleIME,           // Not present in Win7
    // ConsolepQueryConsoleIME,                // Added only in Vista and Win2k8, not present in Win7
    ConsolepGetLangId,
    ConsolepAttach,                         // Not present in Win7
    ConsolepGetSelectionInfo,
    ConsolepGetProcessList,

    ConsolepGetHistory,                     // Added in Vista+
    ConsolepSetHistory,                     // Added in Vista+
    // ConsolepSetCurrentFont,                 // Added in Vista+
    // ConsolepSetScreenBufferInfo,            // Added in Vista+
    // ConsolepClientConnect,                  // Added in Win7

    ConsolepMaxApiNumber
} CONSRV_API_NUMBER, *PCONSRV_API_NUMBER;

//
// See https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/ns-shlobj_core-nt_console_props
//
typedef struct _CONSOLE_PROPERTIES
{
    INT   IconIndex;
    LPC_PTRTYPE(HICON) hIcon;
    LPC_PTRTYPE(HICON) hIconSm;
    DWORD dwHotKey;
    DWORD dwStartupFlags;

    // NT_CONSOLE_PROPS
    WORD wFillAttribute;
    WORD wPopupFillAttribute;

    //
    // Not on MSDN, but show up in binary
    //
    WORD wShowWindow;
    WORD wUnknown;

    COORD dwScreenBufferSize;
    COORD dwWindowSize;
    COORD dwWindowOrigin;
    DWORD nFont;
    DWORD nInputBufferSize;
    COORD dwFontSize;
    UINT  uFontFamily;
    UINT  uFontWeight;
    WCHAR FaceName[LF_FACESIZE];
    UINT  uCursorSize;
    BOOL  bFullScreen;
    BOOL  bQuickEdit;
    BOOL  bInsertMode;
    BOOL  bAutoPosition;
    UINT  uHistoryBufferSize;
    UINT  uNumberOfHistoryBuffers;
    BOOL  bHistoryNoDup;
    COLORREF ColorTable[16];

    // NT_FE_CONSOLE_PROPS
    UINT uCodePage;
} CONSOLE_PROPERTIES;

enum
{
    INIT_SUCCESS, // STATUS_WAIT_0
    INIT_FAILURE, // STATUS_WAIT_1
    MAX_INIT_EVENTS
};

typedef struct _CONSOLE_START_INFO
{
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE InputWaitHandle;
    LPC_HANDLE InputHandle;
    LPC_HANDLE OutputHandle;
    LPC_HANDLE ErrorHandle;
    LPC_HANDLE InitEvents[MAX_INIT_EVENTS];

    CONSOLE_PROPERTIES;
} CONSOLE_START_INFO, *PCONSOLE_START_INFO;

#if defined(_M_IX86) && !defined(USE_LPC6432)
C_ASSERT(sizeof(CONSOLE_START_INFO) == 0xFC);
#endif

typedef struct _CONSRV_API_CONNECTINFO
{
    CONSOLE_START_INFO ConsoleStartInfo;

    BOOLEAN IsConsoleApp;
    BOOLEAN IsWindowVisible;

    // USHORT Padding;

    LPC_PTRTYPE(LPTHREAD_START_ROUTINE) CtrlRoutine;
    LPC_PTRTYPE(LPTHREAD_START_ROUTINE) PropRoutine;
    LPC_PTRTYPE(LPTHREAD_START_ROUTINE) ImeRoutine;

    ULONG  TitleLength;
    WCHAR  ConsoleTitle[MAX_PATH + 1];  // Console title or full path to the startup shortcut
    ULONG  DesktopLength;
    LPC_PTR(WCHAR) Desktop;                     // Contrary to the case of CONSOLE_ALLOCCONSOLE, the
                                        // desktop string is allocated in the process' heap,
                                        // and CSR will read it via NtReadVirtualMemory.
    ULONG  AppNameLength;
    WCHAR  AppName[128];                // Full path of the launched app
    ULONG  CurDirLength;
    WCHAR  CurDir[MAX_PATH + 1];
} CONSRV_API_CONNECTINFO, *PCONSRV_API_CONNECTINFO;

#if defined(_M_IX86) && !defined(USE_LPC6432)
C_ASSERT(sizeof(CONSRV_API_CONNECTINFO) == 0x638);
#elif defined(_M_IX86) || defined(_M_AMD64)
C_ASSERT(sizeof(CONSRV_API_CONNECTINFO) == 0x680);
#endif

typedef struct _CONSOLE_GETPROCESSLIST
{
    LPC_HANDLE ConsoleHandle;
    ULONG  ProcessCount;
    LPC_PTR(DWORD) ProcessIdsList;
} CONSOLE_GETPROCESSLIST, *PCONSOLE_GETPROCESSLIST;

typedef struct _CONSOLE_GENERATECTRLEVENT
{
    LPC_HANDLE ConsoleHandle;
    ULONG  CtrlEvent;
    ULONG  ProcessGroupId;
} CONSOLE_GENERATECTRLEVENT, *PCONSOLE_GENERATECTRLEVENT;

typedef struct _CONSOLE_NOTIFYLASTCLOSE
{
    LPC_HANDLE ConsoleHandle;
} CONSOLE_NOTIFYLASTCLOSE, *PCONSOLE_NOTIFYLASTCLOSE;



typedef struct _CONSOLE_WRITECONSOLE
{
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE OutputHandle;

    CHAR  StaticBuffer[80];
    LPC_PVOID Buffer; // BufPtr
    ULONG NumBytes;

    // On Windows, the client never uses this member
    ULONG Reserved1;

    BOOLEAN UsingStaticBuffer;
    BOOLEAN Unicode;

    // On Windows, the client never uses this member
    CHAR Reserved2[6];
} CONSOLE_WRITECONSOLE, *PCONSOLE_WRITECONSOLE;

typedef struct _CONSOLE_READCONSOLE
{
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE InputHandle;

    USHORT ExeLength;

    CHAR  StaticBuffer[80];
    LPC_PVOID Buffer; // BufPtr
    ULONG NumBytes;

    ULONG CaptureBufferSize;

    ULONG   InitialNumBytes;
    ULONG   CtrlWakeupMask;
    ULONG   ControlKeyState;
    BOOLEAN Unicode;
} CONSOLE_READCONSOLE, *PCONSOLE_READCONSOLE;

typedef struct _CONSOLE_ALLOCCONSOLE
{
    LPC_PTR(CONSOLE_START_INFO) ConsoleStartInfo;

    ULONG  TitleLength;
    LPC_PTR(WCHAR) ConsoleTitle;    // Console title or full path to the startup shortcut
    ULONG  DesktopLength;
    LPC_PTR(WCHAR) Desktop;
    ULONG  AppNameLength;
    LPC_PTR(WCHAR) AppName;         // Full path of the launched app
    ULONG  CurDirLength;
    LPC_PTR(WCHAR) CurDir;

    LPC_PTRTYPE(LPTHREAD_START_ROUTINE) CtrlRoutine;
    LPC_PTRTYPE(LPTHREAD_START_ROUTINE) PropRoutine;
} CONSOLE_ALLOCCONSOLE, *PCONSOLE_ALLOCCONSOLE;

typedef struct _CONSOLE_ATTACHCONSOLE
{
    /*
     * If ProcessId == ATTACH_PARENT_PROCESS == -1, then attach
     * the current process to its parent process console.
     */
    ULONG ProcessId;

    LPC_PTR(CONSOLE_START_INFO) ConsoleStartInfo;

    LPC_PTRTYPE(LPTHREAD_START_ROUTINE) CtrlRoutine;
    LPC_PTRTYPE(LPTHREAD_START_ROUTINE) PropRoutine;
} CONSOLE_ATTACHCONSOLE, *PCONSOLE_ATTACHCONSOLE;

typedef struct _CONSOLE_FREECONSOLE
{
    LPC_HANDLE ConsoleHandle;
} CONSOLE_FREECONSOLE, *PCONSOLE_FREECONSOLE;

typedef struct _CONSOLE_GETSCREENBUFFERINFO
{
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE OutputHandle;
    COORD  ScreenBufferSize;
    COORD  CursorPosition;
    COORD  ViewOrigin;
    WORD   Attributes;
    COORD  ViewSize;
    COORD  MaximumViewSize;
} CONSOLE_GETSCREENBUFFERINFO, *PCONSOLE_GETSCREENBUFFERINFO;

typedef struct _CONSOLE_SETCURSORPOSITION
{
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE OutputHandle;
    COORD  Position;
} CONSOLE_SETCURSORPOSITION, *PCONSOLE_SETCURSORPOSITION;

typedef struct _CONSOLE_SHOWCURSOR
{
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE OutputHandle;
    BOOL   Show;
    INT    RefCount;
} CONSOLE_SHOWCURSOR, *PCONSOLE_SHOWCURSOR;

typedef struct _CONSOLE_SETCURSOR
{
    LPC_HANDLE  ConsoleHandle;
    LPC_HANDLE  OutputHandle;
    HCURSOR CursorHandle;
} CONSOLE_SETCURSOR, *PCONSOLE_SETCURSOR;

typedef struct _CONSOLE_GETSETCURSORINFO
{
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE OutputHandle;
    CONSOLE_CURSOR_INFO Info;
/*
    ULONG   Size;
    BOOLEAN Visible;
*/
} CONSOLE_GETSETCURSORINFO, *PCONSOLE_GETSETCURSORINFO;

typedef struct _CONSOLE_GETMOUSEINFO
{
    LPC_HANDLE ConsoleHandle;
    ULONG  NumButtons;
} CONSOLE_GETMOUSEINFO, *PCONSOLE_GETMOUSEINFO;

typedef struct _CONSOLE_SETTEXTATTRIB
{
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE OutputHandle;
    WORD   Attributes;
} CONSOLE_SETTEXTATTRIB, *PCONSOLE_SETTEXTATTRIB;

typedef struct _CONSOLE_GETSETCONSOLEMODE
{
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE Handle;
    ULONG  Mode;
} CONSOLE_GETSETCONSOLEMODE, *PCONSOLE_GETSETCONSOLEMODE;

typedef struct _CONSOLE_GETDISPLAYMODE
{
    LPC_HANDLE ConsoleHandle;
    ULONG  DisplayMode; // ModeFlags
} CONSOLE_GETDISPLAYMODE, *PCONSOLE_GETDISPLAYMODE;

typedef struct _CONSOLE_SETDISPLAYMODE
{
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE OutputHandle;
    ULONG  DisplayMode; // ModeFlags
    COORD  NewSBDim;
    LPC_HANDLE EventHandle;
} CONSOLE_SETDISPLAYMODE, *PCONSOLE_SETDISPLAYMODE;

/*
 * Console hardware states.
 */
#define CONSOLE_HARDWARE_STATE_GDI_MANAGED 0
#define CONSOLE_HARDWARE_STATE_DIRECT      1

typedef struct _CONSOLE_GETSETHWSTATE
{
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE OutputHandle;
    ULONG  Flags;
    ULONG  State;
} CONSOLE_GETSETHWSTATE, *PCONSOLE_GETSETHWSTATE;


typedef struct _CONSOLE_GETNUMFONTS
{
    LPC_HANDLE ConsoleHandle;
    ULONG  NumFonts;
} CONSOLE_GETNUMFONTS, *PCONSOLE_GETNUMFONTS;

typedef struct _CONSOLE_GETFONTINFO
{
    LPC_HANDLE  ConsoleHandle;
    LPC_HANDLE  OutputHandle;
    BOOLEAN MaximumWindow;
    LPC_PTR(CONSOLE_FONT_INFO) FontInfo;
    ULONG   NumFonts;
} CONSOLE_GETFONTINFO, *PCONSOLE_GETFONTINFO;

typedef struct _CONSOLE_GETFONTSIZE
{
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE OutputHandle;
    ULONG  FontIndex;
    COORD  FontSize;
} CONSOLE_GETFONTSIZE, *PCONSOLE_GETFONTSIZE;

typedef struct _CONSOLE_GETCURRENTFONT
{
    LPC_HANDLE  ConsoleHandle;
    LPC_HANDLE  OutputHandle;
    BOOLEAN MaximumWindow;
    ULONG   FontIndex;
    COORD   FontSize;
} CONSOLE_GETCURRENTFONT, *PCONSOLE_GETCURRENTFONT;

typedef struct _CONSOLE_SETFONT
{
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE OutputHandle;
    ULONG  FontIndex;
} CONSOLE_SETFONT, *PCONSOLE_SETFONT;



typedef struct _CONSOLE_CREATESCREENBUFFER
{
    LPC_HANDLE ConsoleHandle;
    ULONG  DesiredAccess; // ACCESS_MASK
    BOOL   InheritHandle;
    ULONG  ShareMode;
    /* Type of the screen buffer: CONSOLE_TEXTMODE_BUFFER or CONSOLE_GRAPHICS_BUFFER */
    ULONG  ScreenBufferType;
    /*
     * This structure holds the initialization information
     * for graphics screen buffers.
     */
    CONSOLE_GRAPHICS_BUFFER_INFO GraphicsBufferInfo;
    LPC_HANDLE hMutex;
    LPC_PVOID  lpBitMap;
    LPC_HANDLE OutputHandle;     /* Handle to newly created screen buffer */
} CONSOLE_CREATESCREENBUFFER, *PCONSOLE_CREATESCREENBUFFER;

typedef struct _CONSOLE_SETACTIVESCREENBUFFER
{
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE OutputHandle;  /* Handle to screen buffer to switch to */
} CONSOLE_SETACTIVESCREENBUFFER, *PCONSOLE_SETACTIVESCREENBUFFER;

typedef struct _CONSOLE_INVALIDATEDIBITS
{
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE OutputHandle;
    SMALL_RECT Region;
} CONSOLE_INVALIDATEDIBITS, *PCONSOLE_INVALIDATEDIBITS;

typedef struct _CONSOLE_SETPALETTE
{
    LPC_HANDLE   ConsoleHandle;
    LPC_HANDLE   OutputHandle;
    LPC_PTRTYPE(HPALETTE) PaletteHandle;
    UINT     Usage;
} CONSOLE_SETPALETTE, *PCONSOLE_SETPALETTE;

typedef struct _CONSOLE_GETSETCONSOLETITLE
{
    LPC_HANDLE  ConsoleHandle;
    ULONG   Length;
    LPC_PVOID   Title;
    BOOLEAN Unicode;
} CONSOLE_GETSETCONSOLETITLE, *PCONSOLE_GETSETCONSOLETITLE;

typedef struct _CONSOLE_FLUSHINPUTBUFFER
{
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE InputHandle;
} CONSOLE_FLUSHINPUTBUFFER, *PCONSOLE_FLUSHINPUTBUFFER;

typedef struct _CONSOLE_SCROLLSCREENBUFFER
{
    LPC_HANDLE     ConsoleHandle;
    LPC_HANDLE     OutputHandle;
    SMALL_RECT ScrollRectangle;
    SMALL_RECT ClipRectangle;
    BOOL       UseClipRectangle;
    COORD      DestinationOrigin;
    CHAR_INFO  Fill;
    BOOLEAN    Unicode;
} CONSOLE_SCROLLSCREENBUFFER, *PCONSOLE_SCROLLSCREENBUFFER;


/*
 * An attribute or a character are instances of the same entity, namely
 * a "code" (what would be called an (ANSI) escape sequence). Therefore
 * encode them inside the same structure.
 */
typedef enum _CODE_TYPE
{
    CODE_ASCII      = 0x01,
    CODE_UNICODE    = 0x02,
    CODE_ATTRIBUTE  = 0x03
} CODE_TYPE;

typedef union _CODE_ELEMENT
{
    CHAR  AsciiChar;
    WCHAR UnicodeChar;
    WORD  Attribute;
} CODE_ELEMENT;

typedef struct _CONSOLE_OUTPUTCODE
{
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE OutputHandle;
    COORD  Coord;

    CODE_TYPE CodeType;
    CHAR      CodeStaticBuffer[80]; // == 40 * sizeof(CODE_ELEMENT)
    LPC_PVOID     pCode; // Either a pointer to a character or to an attribute.
    // union
    // {
        // LPC_PVOID  pCode;
        // PCHAR  AsciiChar;
        // LPC_PTR(WCHAR) UnicodeChar;
        // PWORD  Attribute;
    // } pCode;    // Either a pointer to a character or to an attribute.

    ULONG NumCodes;
} CONSOLE_READOUTPUTCODE , *PCONSOLE_READOUTPUTCODE,
  CONSOLE_WRITEOUTPUTCODE, *PCONSOLE_WRITEOUTPUTCODE;

typedef struct _CONSOLE_FILLOUTPUTCODE
{
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE OutputHandle;
    COORD  WriteCoord;

    CODE_TYPE    CodeType;
    CODE_ELEMENT Code; // Either a character or an attribute.

    ULONG NumCodes;
} CONSOLE_FILLOUTPUTCODE, *PCONSOLE_FILLOUTPUTCODE;

typedef struct _CONSOLE_GETINPUT
{
    LPC_HANDLE        ConsoleHandle;
    LPC_HANDLE        InputHandle;
    INPUT_RECORD  RecordStaticBuffer[5];
    LPC_PTR(INPUT_RECORD) RecordBufPtr;
    ULONG         NumRecords;
    WORD          Flags;
    BOOLEAN       Unicode;
} CONSOLE_GETINPUT, *PCONSOLE_GETINPUT;

typedef struct _CONSOLE_WRITEINPUT
{
    LPC_HANDLE        ConsoleHandle;
    LPC_HANDLE        InputHandle;
    INPUT_RECORD  RecordStaticBuffer[5];
    LPC_PTR(INPUT_RECORD) RecordBufPtr;
    ULONG         NumRecords;
    BOOLEAN       Unicode;
    BOOLEAN       AppendToEnd;
} CONSOLE_WRITEINPUT, *PCONSOLE_WRITEINPUT;

typedef struct _CONSOLE_READOUTPUT
{
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE OutputHandle;

    CHAR_INFO  StaticBuffer;
    LPC_PTR(CHAR_INFO) CharInfo;

    SMALL_RECT ReadRegion;
    BOOLEAN Unicode;
} CONSOLE_READOUTPUT, *PCONSOLE_READOUTPUT;

typedef struct _CONSOLE_WRITEOUTPUT
{
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE OutputHandle;

    CHAR_INFO  StaticBuffer;
    LPC_PTR(CHAR_INFO) CharInfo;

    SMALL_RECT WriteRegion;
    BOOLEAN Unicode;

    /*
     * If we are going to write too large (>= 64 kB, size of the CSR heap)
     * data buffers, we allocate a heap buffer in the process' memory, and
     * CSR will read it via NtReadVirtualMemory.
     */
    BOOLEAN UseVirtualMemory;
} CONSOLE_WRITEOUTPUT, *PCONSOLE_WRITEOUTPUT;

typedef struct _CONSOLE_GETNUMINPUTEVENTS
{
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE InputHandle;
    ULONG  NumberOfEvents;
} CONSOLE_GETNUMINPUTEVENTS, *PCONSOLE_GETNUMINPUTEVENTS;



typedef struct _CONSOLE_CLOSEHANDLE
{
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE Handle;
} CONSOLE_CLOSEHANDLE, *PCONSOLE_CLOSEHANDLE;

typedef struct _CONSOLE_VERIFYHANDLE
{
    BOOL   IsValid;
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE Handle;
} CONSOLE_VERIFYHANDLE, *PCONSOLE_VERIFYHANDLE;

typedef struct _CONSOLE_DUPLICATEHANDLE
{
    LPC_HANDLE  ConsoleHandle;
    LPC_HANDLE  SourceHandle;
    ULONG   DesiredAccess; // ACCESS_MASK
    BOOLEAN InheritHandle;
    ULONG   Options;
    LPC_HANDLE  TargetHandle;
} CONSOLE_DUPLICATEHANDLE, *PCONSOLE_DUPLICATEHANDLE;

typedef struct _CONSOLE_GETHANDLEINFO
{
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE Handle;
    ULONG  Flags;
} CONSOLE_GETHANDLEINFO, *PCONSOLE_GETHANDLEINFO;

typedef struct _CONSOLE_SETHANDLEINFO
{
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE Handle;
    ULONG  Mask;
    ULONG  Flags;
} CONSOLE_SETHANDLEINFO, *PCONSOLE_SETHANDLEINFO;

/*
 * Type of handles.
 */
typedef enum _CONSOLE_HANDLE_TYPE
{
    HANDLE_INPUT    = 0x01,
    HANDLE_OUTPUT   = 0x02
} CONSOLE_HANDLE_TYPE;

typedef struct _CONSOLE_OPENCONSOLE
{
    LPC_HANDLE ConsoleHandle;
    CONSOLE_HANDLE_TYPE HandleType;
    ULONG  DesiredAccess; // ACCESS_MASK
    BOOL   InheritHandle;
    ULONG  ShareMode;
    LPC_HANDLE Handle;
} CONSOLE_OPENCONSOLE, *PCONSOLE_OPENCONSOLE;



typedef struct _CONSOLE_GETLARGESTWINDOWSIZE
{
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE OutputHandle;
    COORD  Size;
} CONSOLE_GETLARGESTWINDOWSIZE, *PCONSOLE_GETLARGESTWINDOWSIZE;

typedef struct _CONSOLE_MENUCONTROL
{
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE OutputHandle;
    ULONG  CmdIdLow;
    ULONG  CmdIdHigh;
    HMENU  MenuHandle;
} CONSOLE_MENUCONTROL, *PCONSOLE_MENUCONTROL;

typedef struct _CONSOLE_SETMENUCLOSE
{
    LPC_HANDLE ConsoleHandle;
    BOOL   Enable;
} CONSOLE_SETMENUCLOSE, *PCONSOLE_SETMENUCLOSE;

typedef struct _CONSOLE_SETWINDOWINFO
{
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE OutputHandle;
    BOOL   Absolute;
    SMALL_RECT WindowRect; // New console window position in the screen-buffer frame (Absolute == TRUE)
                           // or in the old window position frame (Absolute == FALSE).
} CONSOLE_SETWINDOWINFO, *PCONSOLE_SETWINDOWINFO;

typedef struct _CONSOLE_GETWINDOW
{
    LPC_HANDLE ConsoleHandle;
    LPC_PTRTYPE(HWND) WindowHandle;
} CONSOLE_GETWINDOW, *PCONSOLE_GETWINDOW;

typedef struct _CONSOLE_SETICON
{
    LPC_HANDLE ConsoleHandle;
    LPC_PTRTYPE(HICON) IconHandle;
} CONSOLE_SETICON, *PCONSOLE_SETICON;



typedef struct _CONSOLE_ADDGETALIAS
{
    LPC_HANDLE  ConsoleHandle;
    USHORT  SourceLength;
    USHORT  TargetLength; // Also used for storing the number of bytes written.
    USHORT  ExeLength;
    LPC_PVOID   Source;
    LPC_PVOID   Target;
    LPC_PVOID   ExeName;
    BOOLEAN Unicode;
    BOOLEAN Unicode2;
} CONSOLE_ADDGETALIAS, *PCONSOLE_ADDGETALIAS;

typedef struct _CONSOLE_GETALLALIASES
{
    LPC_HANDLE  ConsoleHandle;
    USHORT  ExeLength;
    LPC_PVOID   ExeName;
    BOOLEAN Unicode;
    BOOLEAN Unicode2;
    ULONG   AliasesBufferLength;
    LPC_PVOID   AliasesBuffer;
} CONSOLE_GETALLALIASES, *PCONSOLE_GETALLALIASES;

typedef struct _CONSOLE_GETALLALIASESLENGTH
{
    LPC_HANDLE  ConsoleHandle;
    USHORT  ExeLength;
    LPC_PVOID   ExeName;
    ULONG   Length;
    BOOLEAN Unicode;
    BOOLEAN Unicode2;
} CONSOLE_GETALLALIASESLENGTH, *PCONSOLE_GETALLALIASESLENGTH;

typedef struct _CONSOLE_GETALIASESEXES
{
    LPC_HANDLE  ConsoleHandle;
    ULONG   Length ; // ExeLength; // ExesLength
    LPC_PVOID   ExeNames;
    BOOLEAN Unicode;
} CONSOLE_GETALIASESEXES, *PCONSOLE_GETALIASESEXES;

typedef struct _CONSOLE_GETALIASESEXESLENGTH
{
    LPC_HANDLE  ConsoleHandle;
    ULONG   Length;
    BOOLEAN Unicode;
} CONSOLE_GETALIASESEXESLENGTH, *PCONSOLE_GETALIASESEXESLENGTH;



typedef struct _CONSOLE_GETCOMMANDHISTORY
{
    LPC_HANDLE  ConsoleHandle;
    ULONG   HistoryLength;
    LPC_PVOID   History;
    USHORT  ExeLength;
    LPC_PVOID   ExeName;
    BOOLEAN Unicode;
    BOOLEAN Unicode2;
} CONSOLE_GETCOMMANDHISTORY, *PCONSOLE_GETCOMMANDHISTORY;

typedef struct _CONSOLE_GETCOMMANDHISTORYLENGTH
{
    LPC_HANDLE  ConsoleHandle;
    ULONG   HistoryLength;
    USHORT  ExeLength;
    LPC_PVOID   ExeName;
    BOOLEAN Unicode;
    BOOLEAN Unicode2;
} CONSOLE_GETCOMMANDHISTORYLENGTH, *PCONSOLE_GETCOMMANDHISTORYLENGTH;

typedef struct _CONSOLE_EXPUNGECOMMANDHISTORY
{
    LPC_HANDLE  ConsoleHandle;
    USHORT  ExeLength;
    LPC_PVOID   ExeName;
    BOOLEAN Unicode;
    BOOLEAN Unicode2;
} CONSOLE_EXPUNGECOMMANDHISTORY, *PCONSOLE_EXPUNGECOMMANDHISTORY;

typedef struct _CONSOLE_GETSETHISTORYINFO
{
    UINT HistoryBufferSize;
    UINT NumberOfHistoryBuffers;
    ULONG dwFlags;
} CONSOLE_GETSETHISTORYINFO, *PCONSOLE_GETSETHISTORYINFO;

typedef struct _CONSOLE_SETHISTORYNUMBERCOMMANDS
{
    LPC_HANDLE  ConsoleHandle;
    ULONG   NumCommands;
    USHORT  ExeLength;
    LPC_PVOID   ExeName;
    BOOLEAN Unicode;
    BOOLEAN Unicode2;
} CONSOLE_SETHISTORYNUMBERCOMMANDS, *PCONSOLE_SETHISTORYNUMBERCOMMANDS;

typedef struct _CONSOLE_SETHISTORYMODE
{
    LPC_HANDLE ConsoleHandle;
    ULONG  Mode;
} CONSOLE_SETHISTORYMODE, *PCONSOLE_SETHISTORYMODE;



typedef struct _CONSOLE_SETSCREENBUFFERSIZE
{
    LPC_HANDLE ConsoleHandle;
    LPC_HANDLE OutputHandle;
    COORD  Size;
} CONSOLE_SETSCREENBUFFERSIZE, *PCONSOLE_SETSCREENBUFFERSIZE;

typedef struct _CONSOLE_GETSELECTIONINFO
{
    LPC_HANDLE ConsoleHandle;
    CONSOLE_SELECTION_INFO Info;
} CONSOLE_GETSELECTIONINFO, *PCONSOLE_GETSELECTIONINFO;

typedef struct _CONSOLE_GETINPUTOUTPUTCP
{
    LPC_HANDLE ConsoleHandle;
    UINT   CodePage;
    BOOL   OutputCP;    // TRUE : Output Code Page ; FALSE : Input Code Page
} CONSOLE_GETINPUTOUTPUTCP, *PCONSOLE_GETINPUTOUTPUTCP;

typedef struct _CONSOLE_SETINPUTOUTPUTCP
{
    LPC_HANDLE ConsoleHandle;
    UINT   CodePage;
    BOOL   OutputCP;    // TRUE : Output Code Page ; FALSE : Input Code Page
    LPC_HANDLE EventHandle;
} CONSOLE_SETINPUTOUTPUTCP, *PCONSOLE_SETINPUTOUTPUTCP;

typedef struct _CONSOLE_GETLANGID
{
    LPC_HANDLE ConsoleHandle;
    LANGID LangId;
} CONSOLE_GETLANGID, *PCONSOLE_GETLANGID;

typedef struct _CONSOLE_GETKBDLAYOUTNAME
{
    LPC_HANDLE ConsoleHandle;
    CHAR   LayoutBuffer[KL_NAMELENGTH * sizeof(WCHAR)]; // Can hold up to 9 wchars
    BOOL   Ansi;
} CONSOLE_GETKBDLAYOUTNAME, *PCONSOLE_GETKBDLAYOUTNAME;

typedef struct _CONSOLE_REGISTERVDM
{
    LPC_HANDLE ConsoleHandle;
    ULONG  RegisterFlags;
    LPC_HANDLE StartHardwareEvent;
    LPC_HANDLE EndHardwareEvent;
    LPC_HANDLE ErrorHardwareEvent;

    /* Unused member */
    ULONG  UnusedVar;

    ULONG  VideoStateLength;
    LPC_PVOID  VideoState;  // PVIDEO_HARDWARE_STATE_HEADER

    /* Unused members */
    LPC_PVOID  UnusedBuffer;
    ULONG  UnusedBufferLength;

    COORD  VDMBufferSize;
    LPC_PVOID  VDMBuffer;
} CONSOLE_REGISTERVDM, *PCONSOLE_REGISTERVDM;

typedef struct _CONSOLE_REGISTERCONSOLEIME
{
    LPC_HANDLE ConsoleHandle;
    LPC_PTRTYPE(HWND) hWnd;
    DWORD dwThreadId;
    DWORD cbDesktop;
    LPWSTR pDesktop;
    DWORD dwAttachToThreadId;
} CONSOLE_REGISTERCONSOLEIME, *PCONSOLE_REGISTERCONSOLEIME;

typedef struct _CONSOLE_UNREGISTERCONSOLEIME
{
    LPC_HANDLE ConsoleHandle;
    DWORD dwThreadId;
} CONSOLE_UNREGISTERCONSOLEIME, *PCONSOLE_UNREGISTERCONSOLEIME;

typedef struct _CONSOLE_API_MESSAGE
{
    PORT_MESSAGE Header;

    PCSR_CAPTURE_BUFFER CsrCaptureData;
    CSR_API_NUMBER ApiNumber;
    NTSTATUS Status;
    ULONG Reserved;
    union
    {
        /* Console initialization and uninitialization */
        CONSOLE_ALLOCCONSOLE AllocConsoleRequest;
        CONSOLE_ATTACHCONSOLE AttachConsoleRequest;
        CONSOLE_FREECONSOLE FreeConsoleRequest;

        /* Processes */
        CONSOLE_GETPROCESSLIST GetProcessListRequest;
        CONSOLE_GENERATECTRLEVENT GenerateCtrlEventRequest;
        CONSOLE_NOTIFYLASTCLOSE NotifyLastCloseRequest;

        /* Handles */
        CONSOLE_OPENCONSOLE OpenConsoleRequest;
        CONSOLE_CLOSEHANDLE CloseHandleRequest;
        CONSOLE_VERIFYHANDLE VerifyHandleRequest;
        CONSOLE_DUPLICATEHANDLE DuplicateHandleRequest;
        CONSOLE_GETHANDLEINFO GetHandleInfoRequest;
        CONSOLE_SETHANDLEINFO SetHandleInfoRequest;

        /* Cursor & Mouse */
        CONSOLE_SHOWCURSOR ShowCursorRequest;
        CONSOLE_SETCURSOR SetCursorRequest;
        CONSOLE_GETSETCURSORINFO CursorInfoRequest;
        CONSOLE_SETCURSORPOSITION SetCursorPositionRequest;
        CONSOLE_GETMOUSEINFO GetMouseInfoRequest;

        /* Screen-buffer */
        CONSOLE_CREATESCREENBUFFER CreateScreenBufferRequest;
        CONSOLE_SETACTIVESCREENBUFFER SetScreenBufferRequest;
        CONSOLE_GETSCREENBUFFERINFO ScreenBufferInfoRequest;
        CONSOLE_SETSCREENBUFFERSIZE SetScreenBufferSizeRequest;
        CONSOLE_SCROLLSCREENBUFFER ScrollScreenBufferRequest;

        CONSOLE_GETSELECTIONINFO GetSelectionInfoRequest;
        CONSOLE_FLUSHINPUTBUFFER FlushInputBufferRequest;

        /* Console mode */
        CONSOLE_GETSETCONSOLEMODE ConsoleModeRequest;
        CONSOLE_GETDISPLAYMODE GetDisplayModeRequest;
        CONSOLE_SETDISPLAYMODE SetDisplayModeRequest;
        CONSOLE_GETSETHWSTATE HardwareStateRequest;

        /* Console fonts */
        CONSOLE_GETNUMFONTS GetNumFontsRequest;
        CONSOLE_GETFONTINFO GetFontInfoRequest;
        CONSOLE_GETFONTSIZE GetFontSizeRequest;
        CONSOLE_GETCURRENTFONT GetCurrentFontRequest;
        CONSOLE_SETFONT SetFontRequest;

        /* Console window */
        CONSOLE_INVALIDATEDIBITS InvalidateDIBitsRequest;
        CONSOLE_SETPALETTE SetPaletteRequest;
        CONSOLE_GETSETCONSOLETITLE TitleRequest;
        CONSOLE_GETLARGESTWINDOWSIZE GetLargestWindowSizeRequest;
        CONSOLE_MENUCONTROL MenuControlRequest;
        CONSOLE_SETMENUCLOSE SetMenuCloseRequest;
        CONSOLE_SETWINDOWINFO SetWindowInfoRequest;
        CONSOLE_GETWINDOW GetWindowRequest;
        CONSOLE_SETICON SetIconRequest;

        /* Read */
        CONSOLE_READCONSOLE ReadConsoleRequest;         // SrvReadConsole / ReadConsole
        CONSOLE_GETINPUT GetInputRequest;               // SrvGetConsoleInput / PeekConsoleInput & ReadConsoleInput
        CONSOLE_READOUTPUT ReadOutputRequest;           // SrvReadConsoleOutput / ReadConsoleOutput
        CONSOLE_READOUTPUTCODE ReadOutputCodeRequest;   // SrvReadConsoleOutputString / ReadConsoleOutputAttribute & ReadConsoleOutputCharacter
        CONSOLE_GETNUMINPUTEVENTS GetNumInputEventsRequest;

        /* Write */
        CONSOLE_WRITECONSOLE WriteConsoleRequest;       // SrvWriteConsole / WriteConsole
        CONSOLE_WRITEINPUT WriteInputRequest;
        CONSOLE_WRITEOUTPUT WriteOutputRequest;
        CONSOLE_WRITEOUTPUTCODE WriteOutputCodeRequest;

        CONSOLE_FILLOUTPUTCODE FillOutputRequest;
        CONSOLE_SETTEXTATTRIB SetTextAttribRequest;

        /* Aliases */
        CONSOLE_ADDGETALIAS ConsoleAliasRequest;
        CONSOLE_GETALLALIASES GetAllAliasesRequest;
        CONSOLE_GETALLALIASESLENGTH GetAllAliasesLengthRequest;
        CONSOLE_GETALIASESEXES GetAliasesExesRequest;
        CONSOLE_GETALIASESEXESLENGTH GetAliasesExesLengthRequest;

        /* History */
        CONSOLE_GETCOMMANDHISTORY GetCommandHistoryRequest;
        CONSOLE_GETCOMMANDHISTORYLENGTH GetCommandHistoryLengthRequest;
        CONSOLE_EXPUNGECOMMANDHISTORY ExpungeCommandHistoryRequest;
        CONSOLE_GETSETHISTORYINFO HistoryInfoRequest;
        CONSOLE_SETHISTORYNUMBERCOMMANDS SetHistoryNumberCommandsRequest;
        CONSOLE_SETHISTORYMODE SetHistoryModeRequest;

        /* Input and Output Code Pages; keyboard */
        CONSOLE_GETINPUTOUTPUTCP GetConsoleCPRequest;
        CONSOLE_SETINPUTOUTPUTCP SetConsoleCPRequest;
        CONSOLE_GETLANGID LangIdRequest;
        CONSOLE_GETKBDLAYOUTNAME GetKbdLayoutNameRequest;

        /* Virtual DOS Machine */
        CONSOLE_REGISTERVDM RegisterVDMRequest;

        /* Console IME */
        CONSOLE_REGISTERCONSOLEIME RegisterConsoleIME;
        CONSOLE_UNREGISTERCONSOLEIME UnregisterConsoleIME;
    } Data;
} CONSOLE_API_MESSAGE, *PCONSOLE_API_MESSAGE;

// Check that a CONSOLE_API_MESSAGE can hold in a CSR_API_MESSAGE.
CHECK_API_MSG_SIZE(CONSOLE_API_MESSAGE);

#endif // _CONMSG_H

/* EOF */
