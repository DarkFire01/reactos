/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     X Window System server for POSIX X clients
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <windows.h>
#include <stdarg.h>

/* X resource id (window, pixmap, GC, ...) */
typedef unsigned long XID;

#define PSX_X11_PIPE_NAME   L"\\\\.\\pipe\\ReactOS-X11-0"

#define SCREEN_W    800
#define SCREEN_H    600

#define ID_ROOT          0x00000002
#define ID_COLORMAP      0x00000020
#define ID_VISUAL        0x00000021
#define ID_CLIENT_BASE   0x00400000
#define ID_CLIENT_STRIDE 0x00200000
#define ID_CLIENT_MASK   0x001FFFFF

#define WHITE_PIXEL   0x00FFFFFF
#define BLACK_PIXEL   0x00000000

/* Tracing */
#define PSXX11_TRACE 1
#if PSXX11_TRACE
static
VOID
XTrace(
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...)
{
    CHAR Buffer[256];
    va_list Args;

    va_start(Args, Format);
    wvsprintfA(Buffer, Format, Args);
    va_end(Args);
    OutputDebugStringA(Buffer);
}
#else
static
VOID
XTrace(
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...)
{
    UNREFERENCED_PARAMETER(Format);
}
#endif

/* X protocol opcodes */
enum
{
    X_CreateWindow = 1, X_ChangeWindowAttributes = 2, X_GetWindowAttributes = 3,
    X_DestroyWindow = 4, X_ReparentWindow = 7, X_MapWindow = 8, X_MapSubwindows = 9,
    X_UnmapWindow = 10, X_UnmapSubwindows = 11,
    X_ConfigureWindow = 12, X_GetGeometry = 14, X_QueryTree = 15, X_InternAtom = 16,
    X_GetAtomName = 17, X_ChangeProperty = 18, X_DeleteProperty = 19, X_GetProperty = 20,
    X_ListProperties = 21, X_SetSelectionOwner = 22, X_GetSelectionOwner = 23,
    X_ConvertSelection = 24,
    X_GrabPointer = 26, X_UngrabPointer = 27, X_GrabButton = 28, X_UngrabButton = 29,
    X_GrabKeyboard = 31, X_UngrabKeyboard = 32, X_GrabKey = 33, X_QueryPointer = 38,
    X_TranslateCoordinates = 40, X_SetInputFocus = 42, X_GetInputFocus = 43,
    X_OpenFont = 45, X_CloseFont = 46, X_QueryFont = 47, X_ListFonts = 49,
    X_CreatePixmap = 53,
    X_FreePixmap = 54, X_CreateGC = 55, X_ChangeGC = 56, X_CopyGC = 57,
    X_SetDashes = 58, X_SetClipRectangles = 59, X_FreeGC = 60, X_ClearArea = 61,
    X_CopyArea = 62, X_CopyPlane = 63, X_PolyPoint = 64, X_PolyLine = 65, X_PolySegment = 66,
    X_PolyRectangle = 67, X_PolyArc = 68, X_FillPoly = 69, X_PolyFillRectangle = 70,
    X_PolyFillArc = 71, X_PutImage = 72, X_PolyText8 = 74, X_PolyText16 = 75,
    X_ImageText8 = 76, X_ImageText16 = 77, X_CreateColormap = 78,
    X_AllocColor = 84, X_AllocNamedColor = 85, X_QueryColors = 91, X_LookupColor = 92,
    X_QueryBestSize = 97, X_QueryExtension = 98, X_GetKeyboardMapping = 101,
    X_GetModifierMapping = 119
};

/* X error codes */
#define BadImplementation 17

typedef struct _X_COLOR_NAME
{
    PCSTR Name;
    BYTE Red;
    BYTE Green;
    BYTE Blue;
} X_COLOR_NAME, *PX_COLOR_NAME;

/* Color names for AllocNamedColor/LookupColor, lowercase with spaces removed */
static const X_COLOR_NAME g_ColorNames[] =
{
    { "black",       0,   0,   0   }, { "white",      255, 255, 255 },
    { "red",         255, 0,   0   }, { "green",      0,   255, 0   },
    { "blue",        0,   0,   255 }, { "yellow",     255, 255, 0   },
    { "cyan",        0,   255, 255 }, { "magenta",    255, 0,   255 },
    { "gray",        190, 190, 190 }, { "grey",       190, 190, 190 },
    { "darkgray",    169, 169, 169 }, { "darkgrey",   169, 169, 169 },
    { "lightgray",   211, 211, 211 }, { "lightgrey",  211, 211, 211 },
    { "dimgray",     105, 105, 105 }, { "dimgrey",    105, 105, 105 },
    { "navy",        0,   0,   128 }, { "navyblue",   0,   0,   128 },
    { "darkgreen",   0,   100, 0   }, { "darkred",    139, 0,   0   },
    { "darkblue",    0,   0,   139 }, { "brown",      165, 42,  42  },
    { "orange",      255, 165, 0   }, { "pink",       255, 192, 203 },
    { "purple",      160, 32,  240 }, { "violet",     238, 130, 238 },
    { "turquoise",   64,  224, 208 }, { "gold",       255, 215, 0   },
    { "steelblue",   70,  130, 180 }, { "skyblue",    135, 206, 235 },
    { "lightblue",   173, 216, 230 }, { "lightyellow", 255, 255, 224 },
    { "wheat",       245, 222, 179 }, { "tan",        210, 180, 140 },
    { "maroon",      176, 48,  96  }, { "salmon",     250, 128, 114 },
    { "khaki",       240, 230, 140 }, { "plum",       221, 160, 221 },
    { "orchid",      218, 112, 214 }, { "coral",      255, 127, 80  },
    { "aquamarine",  127, 255, 212 }, { "chartreuse", 127, 255, 0   },
    { "firebrick",   178, 34,  34  }, { "goldenrod",  218, 165, 32  },
    { "seagreen",    46,  139, 87  }, { "slateblue",  106, 90,  205 },
    { "slategray",   112, 128, 144 }, { "ivory",      255, 255, 240 },
    { "snow",        255, 250, 250 }, { "beige",      245, 245, 220 },
    /* CDE/Motif and other common rgb.txt colors */
    { "cornflowerblue", 100, 149, 237 }, { "royalblue",   65,  105, 225 },
    { "midnightblue", 25,  25,  112 }, { "dodgerblue",  30,  144, 255 },
    { "deepskyblue", 0,   191, 255 }, { "lightsteelblue", 176, 196, 222 },
    { "powderblue",  176, 224, 230 }, { "cadetblue",   95,  158, 160 },
    { "mediumblue",  0,   0,   205 }, { "lightcyan",   224, 255, 255 },
    { "forestgreen", 34,  139, 34  }, { "limegreen",   50,  205, 50  },
    { "lawngreen",   124, 252, 0   }, { "palegreen",   152, 251, 152 },
    { "springgreen", 0,   255, 127 }, { "olivedrab",   107, 142, 35  },
    { "darkolivegreen", 85, 107, 47 }, { "yellowgreen", 154, 205, 50 },
    { "greenyellow", 173, 255, 47  }, { "lightgreen",  144, 238, 144 },
    { "tomato",      255, 99,  71  }, { "orangered",   255, 69,  0   },
    { "hotpink",     255, 105, 180 }, { "deeppink",    255, 20,  147 },
    { "lightpink",   255, 182, 193 }, { "indianred",   205, 92,  92  },
    { "sienna",      160, 82,  45  }, { "chocolate",   210, 105, 30  },
    { "sandybrown",  244, 164, 96  }, { "peru",        205, 133, 63  },
    { "burlywood",   222, 184, 135 }, { "rosybrown",   188, 143, 143 },
    { "darkgoldenrod", 184, 134, 11 }, { "lightgoldenrod", 238, 221, 130 },
    { "mediumorchid", 186, 85, 211 }, { "darkorchid",  153, 50,  204 },
    { "mediumpurple", 147, 112, 219 }, { "thistle",    216, 191, 216 },
    { "lavender",    230, 230, 250 }, { "mediumseagreen", 60, 179, 113 },
    { "darkseagreen", 143, 188, 143 }, { "lightseagreen", 32, 178, 170 },
    { "mediumturquoise", 72, 209, 204 }, { "paleturquoise", 175, 238, 238 },
    { "gainsboro",   220, 220, 220 }, { "whitesmoke",  245, 245, 245 },
    { "honeydew",    240, 255, 240 }, { "azure",       240, 255, 255 },
    { "mintcream",   245, 255, 250 }, { "ghostwhite",  248, 248, 255 },
    { "aliceblue",   240, 248, 255 }, { "lavenderblush", 255, 240, 245 },
    { "seashell",    255, 245, 238 }, { "linen",       250, 240, 230 },
    { "oldlace",     253, 245, 230 }, { "cornsilk",    255, 248, 220 },
    { "lemonchiffon", 255, 250, 205 }, { "lightgoldenrodyellow", 250, 250, 210 },
    { "moccasin",    255, 228, 181 }, { "navajowhite", 255, 222, 173 },
    { "peachpuff",   255, 218, 185 }, { "mistyrose",   255, 228, 225 },
    { "gray10", 26, 26, 26 }, { "gray20", 51, 51, 51 }, { "gray30", 77, 77, 77 },
    { "gray40", 102, 102, 102 }, { "gray50", 127, 127, 127 }, { "gray60", 153, 153, 153 },
    { "gray70", 179, 179, 179 }, { "gray80", 204, 204, 204 }, { "gray90", 229, 229, 229 },
    { "grey10", 26, 26, 26 }, { "grey20", 51, 51, 51 }, { "grey30", 77, 77, 77 },
    { "grey40", 102, 102, 102 }, { "grey50", 127, 127, 127 }, { "grey60", 153, 153, 153 },
    { "grey70", 179, 179, 179 }, { "grey80", 204, 204, 204 }, { "grey90", 229, 229, 229 },
};

/**
 * @brief Looks up an X color name. Case and blanks are ignored; unknown names yield white.
 */
static
BOOL
LookupColorName(
    _In_reads_(Length) PCSTR Name,
    _In_ INT Length,
    _Out_ PWORD Red,
    _Out_ PWORD Green,
    _Out_ PWORD Blue)
{
    CHAR Clean[32];
    INT Index;
    INT Entry;
    INT CleanLength = 0;

    for (Index = 0; Index < Length && CleanLength < (INT)sizeof(Clean) - 1; Index++)
    {
        CHAR Char = Name[Index];

        if (Char == ' ' || Char == '\t')
            continue;
        if (Char >= 'A' && Char <= 'Z')
            Char += 'a' - 'A';
        Clean[CleanLength++] = Char;
    }
    Clean[CleanLength] = '\0';

    for (Entry = 0; Entry < (INT)(sizeof(g_ColorNames) / sizeof(g_ColorNames[0])); Entry++)
    {
        if (lstrcmpA(Clean, g_ColorNames[Entry].Name) == 0)
        {
            *Red = (WORD)(g_ColorNames[Entry].Red << 8 | g_ColorNames[Entry].Red);
            *Green = (WORD)(g_ColorNames[Entry].Green << 8 | g_ColorNames[Entry].Green);
            *Blue = (WORD)(g_ColorNames[Entry].Blue << 8 | g_ColorNames[Entry].Blue);
            return TRUE;
        }
    }

    *Red = *Green = *Blue = 0xFFFF;
    return FALSE;
}

/* X event codes */
enum
{
    KeyPress = 2, KeyRelease = 3, ButtonPress = 4, ButtonRelease = 5,
    MotionNotify = 6, EnterNotify = 7, LeaveNotify = 8, Expose = 12, NoExpose = 14,
    CreateNotify = 16, DestroyNotify = 17, UnmapNotify = 18, MapNotify = 19,
    MapRequest = 20, ReparentNotify = 21, ConfigureNotify = 22,
    ConfigureRequest = 23, PropertyNotify = 28, SelectionRequest = 30,
    SelectionNotify = 31
};

/* Event mask bits */
#define KeyPressMask             0x00000001
#define KeyReleaseMask           0x00000002
#define ButtonPressMask          0x00000004
#define ButtonReleaseMask        0x00000008
#define EnterWindowMask          0x00000010
#define LeaveWindowMask          0x00000020
#define PointerMotionMask        0x00000040
#define ExposureMask             0x00008000
#define StructureNotifyMask      0x00020000
#define SubstructureNotifyMask   0x00080000
#define SubstructureRedirectMask 0x00100000
#define PropertyChangeMask       0x00400000

/* CreateWindow/ChangeWindowAttributes value bits */
#define CWBackPixmap       0x0001
#define CWBackPixel        0x0002
#define CWBorderPixmap     0x0004
#define CWBorderPixel      0x0008
#define CWOverrideRedirect 0x0200
#define CWEventMask        0x0800

/* GC component bits, in core protocol order */
#define GCFunction          0x00000001
#define GCPlaneMask         0x00000002
#define GCForeground        0x00000004
#define GCBackground        0x00000008
#define GCLineWidth         0x00000010
#define GCLineStyle         0x00000020
#define GCCapStyle          0x00000040
#define GCJoinStyle         0x00000080
#define GCFillStyle         0x00000100
#define GCFillRule          0x00000200
#define GCTile              0x00000400
#define GCStipple           0x00000800
#define GCTileStipXOrigin   0x00001000
#define GCTileStipYOrigin   0x00002000
#define GCFont              0x00004000
#define GCSubwindowMode     0x00008000
#define GCGraphicsExposures 0x00010000
#define GCClipXOrigin       0x00020000
#define GCClipYOrigin       0x00040000
#define GCClipMask          0x00080000
#define GCDashOffset        0x00100000
#define GCDashList          0x00200000
#define GCArcMode           0x00400000

/* GC raster functions */
#define GXclear   0x0
#define GXcopy    0x3
#define GXxor     0x6
#define GXinvert  0xa

/* ConfigureWindow value bits */
#define CWConfX       0x01
#define CWConfY       0x02
#define CWConfW       0x04
#define CWConfH       0x08
#define CWConfBorder  0x10
#define CWConfSibling 0x20
#define CWConfStack   0x40

/* Wire structures */
#pragma pack(push, 1)
typedef struct _X_REQ_HEAD
{
    BYTE opcode;
    BYTE data;
    WORD length;
} X_REQ_HEAD, *PX_REQ_HEAD;
typedef const X_REQ_HEAD *PCX_REQ_HEAD;

typedef struct _X_RECT
{
    SHORT x, y;
    WORD width, height;
} X_RECT, *PX_RECT;
typedef const X_RECT *PCX_RECT;

typedef struct _X_POINT
{
    SHORT x, y;
} X_POINT, *PX_POINT;
typedef const X_POINT *PCX_POINT;

typedef struct _X_SEGMENT
{
    SHORT x1, y1, x2, y2;
} X_SEGMENT, *PX_SEGMENT;
typedef const X_SEGMENT *PCX_SEGMENT;

typedef struct _X_ARC
{
    SHORT x, y;
    WORD width, height;
    SHORT angle1, angle2;
} X_ARC, *PX_ARC;
typedef const X_ARC *PCX_ARC;
#pragma pack(pop)

/* Client connection states */
#define CS_HANDSHAKE 0
#define CS_RUNNING   1

typedef struct _X_CLIENT
{
    struct _X_CLIENT *Next;
    HANDLE Pipe;
    ULONG Id;
    BOOL Alive;
    INT State;
    WORD Sequence;
    DWORD IdBase;
    DWORD IdMask;

    /* Overlapped read */
    OVERLAPPED ReadOverlapped;
    HANDLE ReadEvent;
    BYTE ReadChunk[8192];
    PBYTE InBuffer;
    DWORD InLength;
    DWORD InCapacity;

    /* Overlapped write */
    OVERLAPPED WriteOverlapped;
    HANDLE WriteEvent;
    BOOL WritePending;
    DWORD WriteInFlight;
    PBYTE OutBuffer;
    DWORD OutLength;
    DWORD OutCapacity;
} X_CLIENT, *PX_CLIENT;

/* Window property such as WM_NAME, WM_NORMAL_HINTS or RESOURCE_MANAGER */
typedef struct _X_PROPERTY
{
    struct _X_PROPERTY *Next;
    XID Atom;
    XID Type;
    BYTE Format;
    DWORD Length;
    PBYTE Data;
} X_PROPERTY, *PX_PROPERTY;

typedef struct _X_SELECTOR
{
    PX_CLIENT Client;
    DWORD Mask;
} X_SELECTOR, *PX_SELECTOR;

#define MAX_SELECT 8
typedef struct _X_WINDOW
{
    struct _X_WINDOW *AllNext;
    XID Id;
    struct _X_WINDOW *Parent;
    struct _X_WINDOW *FirstChild;
    struct _X_WINDOW *LastChild;
    struct _X_WINDOW *PrevSibling;
    struct _X_WINDOW *NextSibling;
    INT X;
    INT Y;
    INT Width;
    INT Height;
    INT BorderWidth;
    DWORD BackgroundPixel;
    DWORD BorderPixel;
    BOOL HasBackground;
    XID BackgroundPixmap;   /* 0 None, 1 ParentRelative, otherwise a pixmap */
    BOOL Mapped;
    BOOL OverrideRedirect;
    X_SELECTOR Selectors[MAX_SELECT];
    INT SelectorCount;
    PX_CLIENT Owner;
    PX_PROPERTY Properties;
} X_WINDOW, *PX_WINDOW;

/* Graphics context with all 23 core components; only what GDI can express is rendered */
#define GC_MAX_DASHES 16
typedef struct _X_GCONTEXT
{
    struct _X_GCONTEXT *Next;
    XID Id;
    DWORD Function;
    DWORD PlaneMask;
    DWORD Foreground;
    DWORD Background;
    DWORD LineWidth;
    DWORD LineStyle;            /* 0 Solid, 1 OnOffDash, 2 DoubleDash */
    DWORD CapStyle;
    DWORD JoinStyle;
    DWORD FillStyle;            /* 0 Solid, 1 Tiled, 2 Stippled, 3 OpaqueStippled */
    DWORD FillRule;
    XID Tile;
    XID Stipple;
    XID Font;
    XID ClipMask;
    INT TileStipXOrigin;
    INT TileStipYOrigin;
    INT ClipXOrigin;
    INT ClipYOrigin;
    DWORD SubwindowMode;        /* 0 ClipByChildren, 1 IncludeInferiors */
    DWORD GraphicsExposures;
    DWORD ArcMode;              /* 0 Chord, 1 PieSlice */
    DWORD DashOffset;
    DWORD DashCount;
    BYTE Dashes[GC_MAX_DASHES];
    PRECT ClipRects;            /* SetClipRectangles list relative to the clip origin */
    INT ClipRectCount;
} X_GCONTEXT, *PX_GCONTEXT;

typedef struct _X_PIXMAP
{
    struct _X_PIXMAP *Next;
    XID Id;
    INT Width;
    INT Height;
    INT Depth;                  /* 1 for bitmaps, otherwise 32 */
    HDC Dc;
    HBITMAP Bitmap;
    PVOID Bits;
} X_PIXMAP, *PX_PIXMAP;

typedef struct _X_BUTTON_GRAB
{
    PX_CLIENT Client;
    XID Window;
    INT Button;
} X_BUTTON_GRAB, *PX_BUTTON_GRAB;

/* Globals */
static HWND g_Hwnd;
static HDC g_ScreenDc;
static HBITMAP g_ScreenDib;
static PVOID g_ScreenBits;
static volatile LONG g_Dirty;
static INT g_FontWidth = 8;
static INT g_FontAscent = 11;
static INT g_FontDescent = 2;
static INT g_FontHeight = 13;

static PX_CLIENT g_Clients;
static ULONG g_ClientSeq;
static PX_WINDOW g_Root;
static PX_WINDOW g_AllWindows;
static PX_GCONTEXT g_GcList;
static PX_PIXMAP g_Pixmaps;
static INT g_PointerX;
static INT g_PointerY;
static XID g_PointerWindowId;
static DWORD g_ServerTime = 1;

/* Pointer grabs: passive button grabs activate on a matching press, XGrabPointer lasts until ungrabbed */
#define MAX_GRABS 16
static X_BUTTON_GRAB g_ButtonGrabs[MAX_GRABS];
static INT g_ButtonGrabCount;
static PX_CLIENT g_PointerGrab;
static XID g_PointerGrabWindow;
static BOOL g_PointerGrabExplicit;

/* Keyboard grab: all key events go to the grabbing client's window */
static PX_CLIENT g_KeyboardGrab;
static XID g_KeyboardGrabWindow;

static
COLORREF
PixelToColor(
    _In_ DWORD Pixel)
{
    return RGB((Pixel >> 16) & 0xFF, (Pixel >> 8) & 0xFF, Pixel & 0xFF);
}

/* Window manager event log, shown newest first in the server window title */
static CHAR g_WmEvents[5][44];
static INT g_WmEventCount;

static
VOID
XWmLog(
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...)
{
    CHAR Title[256];
    va_list Args;
    INT Slot = g_WmEventCount % 5;

    va_start(Args, Format);
    wvsprintfA(g_WmEvents[Slot], Format, Args);
    va_end(Args);
    g_WmEventCount++;
    wsprintfA(Title,
              "X :0 [%s] [%s] [%s] [%s] [%s]",
              g_WmEvents[Slot],
              g_WmEvents[(Slot + 4) % 5],
              g_WmEvents[(Slot + 3) % 5],
              g_WmEvents[(Slot + 2) % 5],
              g_WmEvents[(Slot + 1) % 5]);
    if (g_Hwnd)
        SetWindowTextA(g_Hwnd, Title);
}

/* Overlapped pipe I/O and per-client output buffering */

static
BOOL
SendSetupReply(
    _In_ PX_CLIENT Client);

static
VOID
DispatchRequest(
    _In_ PX_CLIENT Client,
    _In_ PCX_REQ_HEAD Header,
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength);

static
VOID
EnsureCapacity(
    _Inout_ PBYTE *Buffer,
    _Inout_ PDWORD Capacity,
    _In_ DWORD Needed)
{
    DWORD NewCapacity;
    PBYTE NewBuffer;

    if (*Capacity >= Needed)
        return;

    NewCapacity = *Capacity ? *Capacity : 8192;
    while (NewCapacity < Needed)
        NewCapacity *= 2;

    if (*Buffer)
        NewBuffer = (PBYTE)HeapReAlloc(GetProcessHeap(), 0, *Buffer, NewCapacity);
    else
        NewBuffer = (PBYTE)HeapAlloc(GetProcessHeap(), 0, NewCapacity);

    if (NewBuffer)
    {
        *Buffer = NewBuffer;
        *Capacity = NewCapacity;
    }
}

/**
 * @brief Starts an overlapped write of pending output unless one is already in flight.
 */
static
VOID
IssueWrite(
    _In_ PX_CLIENT Client)
{
    if (!Client->Alive || Client->WritePending || Client->OutLength == 0)
        return;

    ResetEvent(Client->WriteEvent);
    RtlZeroMemory(&Client->WriteOverlapped, sizeof(Client->WriteOverlapped));
    Client->WriteOverlapped.hEvent = Client->WriteEvent;
    Client->WriteInFlight = Client->OutLength;

    if (WriteFile(Client->Pipe, Client->OutBuffer, Client->OutLength, NULL, &Client->WriteOverlapped))
    {
        /* Completed synchronously; the event still signals OnWriteDone */
        Client->WritePending = TRUE;
    }
    else if (GetLastError() == ERROR_IO_PENDING)
    {
        Client->WritePending = TRUE;
    }
    else
    {
        XWmLog("c%lu KILL wr e=%lu", Client->Id, GetLastError());
        Client->Alive = FALSE;
    }
}

/**
 * @brief Appends bytes to a client's output buffer. Never blocks.
 */
static
VOID
SendToClient(
    _In_opt_ PX_CLIENT Client,
    _In_reads_bytes_(Length) const VOID *Buffer,
    _In_ DWORD Length)
{
    if (!Client || !Client->Alive)
        return;

    EnsureCapacity(&Client->OutBuffer, &Client->OutCapacity, Client->OutLength + Length);
    if (Client->OutCapacity < Client->OutLength + Length)
    {
        Client->Alive = FALSE;
        return;
    }

    RtlCopyMemory(Client->OutBuffer + Client->OutLength, Buffer, Length);
    Client->OutLength += Length;
    IssueWrite(Client);
}

static
VOID
OnWriteDone(
    _In_ PX_CLIENT Client)
{
    DWORD Written = 0;

    Client->WritePending = FALSE;
    if (!GetOverlappedResult(Client->Pipe, &Client->WriteOverlapped, &Written, FALSE))
    {
        if (GetLastError() != ERROR_IO_INCOMPLETE)
        {
            XWmLog("c%lu KILL wrd e=%lu", Client->Id, GetLastError());
            Client->Alive = FALSE;
        }
        return;
    }

    if (Written > Client->OutLength)
        Written = Client->OutLength;
    if (Written < Client->OutLength)
        RtlMoveMemory(Client->OutBuffer, Client->OutBuffer + Written, Client->OutLength - Written);
    Client->OutLength -= Written;

    /* Flush anything appended while the write was in flight */
    IssueWrite(Client);
    if (!Client->WritePending)
        ResetEvent(Client->WriteEvent);
}

/**
 * @brief Consumes the connection setup and every complete request in the input buffer.
 */
static
VOID
ProcessInput(
    _In_ PX_CLIENT Client)
{
    if (Client->State == CS_HANDSHAKE)
    {
        DWORD NameLength;
        DWORD DataLength;
        DWORD Needed;

        if (Client->InLength < 12)
            return;

        /* Only little-endian clients are supported */
        if (Client->InBuffer[0] != 'l')
        {
            Client->Alive = FALSE;
            return;
        }

        NameLength = Client->InBuffer[6] | (Client->InBuffer[7] << 8);
        DataLength = Client->InBuffer[8] | (Client->InBuffer[9] << 8);
        Needed = 12 + ((NameLength + 3u) & ~3u) + ((DataLength + 3u) & ~3u);
        if (Client->InLength < Needed)
            return;

        RtlMoveMemory(Client->InBuffer, Client->InBuffer + Needed, Client->InLength - Needed);
        Client->InLength -= Needed;
        SendSetupReply(Client);
        Client->State = CS_RUNNING;
        XTrace("psxx11: client %lu handshake ok\n", Client->Id);
    }

    while (Client->State == CS_RUNNING && Client->InLength >= 4)
    {
        DWORD RequestLength = ((DWORD)(Client->InBuffer[2] | (Client->InBuffer[3] << 8))) * 4;
        X_REQ_HEAD Header;

        if (RequestLength < 4)
        {
            XWmLog("c%lu KILL len0 op=%u", Client->Id, Client->InBuffer[0]);
            Client->Alive = FALSE;
            return;
        }

        /* Wait for the rest of this request */
        if (Client->InLength < RequestLength)
            break;

        Header.opcode = Client->InBuffer[0];
        Header.data = Client->InBuffer[1];
        Header.length = (WORD)(Client->InBuffer[2] | (Client->InBuffer[3] << 8));
        Client->Sequence++;
        DispatchRequest(Client, &Header, Client->InBuffer + 4, RequestLength - 4);
        RtlMoveMemory(Client->InBuffer, Client->InBuffer + RequestLength, Client->InLength - RequestLength);
        Client->InLength -= RequestLength;
    }
}

static
VOID
IssueRead(
    _In_ PX_CLIENT Client)
{
    DWORD Read;

    if (!Client->Alive)
        return;

    ResetEvent(Client->ReadEvent);
    RtlZeroMemory(&Client->ReadOverlapped, sizeof(Client->ReadOverlapped));
    Client->ReadOverlapped.hEvent = Client->ReadEvent;
    if (!ReadFile(Client->Pipe, Client->ReadChunk, sizeof(Client->ReadChunk), &Read, &Client->ReadOverlapped) &&
        GetLastError() != ERROR_IO_PENDING)
    {
        XWmLog("c%lu KILL rdi e=%lu", Client->Id, GetLastError());
        Client->Alive = FALSE;
    }
}

static
VOID
OnReadDone(
    _In_ PX_CLIENT Client)
{
    DWORD Read = 0;

    if (!GetOverlappedResult(Client->Pipe, &Client->ReadOverlapped, &Read, FALSE))
    {
        if (GetLastError() != ERROR_IO_INCOMPLETE)
        {
            XWmLog("c%lu KILL rd e=%lu", Client->Id, GetLastError());
            Client->Alive = FALSE;
        }
        return;
    }

    if (Read == 0)
    {
        XWmLog("c%lu KILL eof", Client->Id);
        Client->Alive = FALSE;
        return;
    }

    EnsureCapacity(&Client->InBuffer, &Client->InCapacity, Client->InLength + Read);
    if (Client->InCapacity < Client->InLength + Read)
    {
        Client->Alive = FALSE;
        return;
    }

    RtlCopyMemory(Client->InBuffer + Client->InLength, Client->ReadChunk, Read);
    Client->InLength += Read;
    ProcessInput(Client);
    if (Client->Alive)
        IssueRead(Client);
}

static
VOID
InitReplyHeader(
    _Out_writes_bytes_(32) PBYTE Reply,
    _In_ PX_CLIENT Client,
    _In_ DWORD ExtraUnits)
{
    RtlZeroMemory(Reply, 32);
    Reply[0] = 1;
    Reply[2] = (BYTE)(Client->Sequence & 0xFF);
    Reply[3] = (BYTE)((Client->Sequence >> 8) & 0xFF);
    *(DWORD *)(Reply + 4) = ExtraUnits;
}

/* Resource tables */

static
PX_WINDOW
FindWindowById(
    _In_ XID Id)
{
    PX_WINDOW Window;

    for (Window = g_AllWindows; Window; Window = Window->AllNext)
    {
        if (Window->Id == Id)
            return Window;
    }
    return NULL;
}

static
PX_GCONTEXT
FindGContext(
    _In_ XID Id)
{
    PX_GCONTEXT Gc;

    for (Gc = g_GcList; Gc; Gc = Gc->Next)
    {
        if (Gc->Id == Id)
            return Gc;
    }
    return NULL;
}

static
PX_PIXMAP
FindPixmap(
    _In_ XID Id)
{
    PX_PIXMAP Pixmap;

    for (Pixmap = g_Pixmaps; Pixmap; Pixmap = Pixmap->Next)
    {
        if (Pixmap->Id == Id)
            return Pixmap;
    }
    return NULL;
}

/**
 * @brief Links a window as the last (topmost) child of its parent.
 */
static
VOID
TreeLink(
    _In_ PX_WINDOW Parent,
    _In_ PX_WINDOW Child)
{
    Child->Parent = Parent;
    Child->NextSibling = NULL;
    Child->PrevSibling = Parent->LastChild;
    if (Parent->LastChild)
        Parent->LastChild->NextSibling = Child;
    else
        Parent->FirstChild = Child;
    Parent->LastChild = Child;
}

static
VOID
TreeUnlink(
    _In_ PX_WINDOW Child)
{
    PX_WINDOW Parent = Child->Parent;

    if (!Parent)
        return;

    if (Child->PrevSibling)
        Child->PrevSibling->NextSibling = Child->NextSibling;
    else
        Parent->FirstChild = Child->NextSibling;

    if (Child->NextSibling)
        Child->NextSibling->PrevSibling = Child->PrevSibling;
    else
        Parent->LastChild = Child->PrevSibling;

    Child->Parent = Child->PrevSibling = Child->NextSibling = NULL;
}

static
PX_WINDOW
AllocateWindow(
    _In_ XID Id,
    _In_opt_ PX_WINDOW Parent,
    _In_ INT X,
    _In_ INT Y,
    _In_ INT Width,
    _In_ INT Height)
{
    PX_WINDOW Window;

    Window = (PX_WINDOW)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Window));
    if (!Window)
        return NULL;

    Window->Id = Id;
    Window->X = X;
    Window->Y = Y;
    Window->Width = Width;
    Window->Height = Height;
    Window->BackgroundPixel = WHITE_PIXEL;
    Window->AllNext = g_AllWindows;
    g_AllWindows = Window;
    if (Parent)
        TreeLink(Parent, Window);
    return Window;
}

/**
 * @brief Computes the absolute screen position of a window's origin.
 */
static
VOID
WindowAbsolutePosition(
    _In_opt_ PX_WINDOW Window,
    _Out_ PINT AbsoluteX,
    _Out_ PINT AbsoluteY)
{
    INT X = 0;
    INT Y = 0;

    while (Window)
    {
        X += Window->X;
        Y += Window->Y;
        Window = Window->Parent;
    }
    *AbsoluteX = X;
    *AbsoluteY = Y;
}

/**
 * @brief Computes a window's absolute rect intersected with all of its ancestors.
 */
static
VOID
WindowVisibleRect(
    _In_ PX_WINDOW Window,
    _Out_ PRECT Rect)
{
    INT AbsX;
    INT AbsY;
    RECT Visible;
    RECT Ancestor;
    PX_WINDOW Parent;

    WindowAbsolutePosition(Window, &AbsX, &AbsY);
    Visible.left = AbsX;
    Visible.top = AbsY;
    Visible.right = AbsX + Window->Width;
    Visible.bottom = AbsY + Window->Height;

    for (Parent = Window->Parent; Parent; Parent = Parent->Parent)
    {
        INT ParentX;
        INT ParentY;

        WindowAbsolutePosition(Parent, &ParentX, &ParentY);
        Ancestor.left = ParentX;
        Ancestor.top = ParentY;
        Ancestor.right = ParentX + Parent->Width;
        Ancestor.bottom = ParentY + Parent->Height;
        if (Visible.left < Ancestor.left)
            Visible.left = Ancestor.left;
        if (Visible.top < Ancestor.top)
            Visible.top = Ancestor.top;
        if (Visible.right > Ancestor.right)
            Visible.right = Ancestor.right;
        if (Visible.bottom > Ancestor.bottom)
            Visible.bottom = Ancestor.bottom;
    }

    if (Visible.right < Visible.left)
        Visible.right = Visible.left;
    if (Visible.bottom < Visible.top)
        Visible.bottom = Visible.top;
    *Rect = Visible;
}

static
BOOL
IsWindowViewable(
    _In_opt_ PX_WINDOW Window)
{
    for (; Window; Window = Window->Parent)
    {
        if (!Window->Mapped)
            return FALSE;
    }
    return TRUE;
}

/**
 * @brief Returns TRUE if Ancestor is an ancestor of Window or the window itself.
 */
static
BOOL
IsAncestor(
    _In_ PX_WINDOW Ancestor,
    _In_opt_ PX_WINDOW Window)
{
    for (; Window; Window = Window->Parent)
    {
        if (Window == Ancestor)
            return TRUE;
    }
    return FALSE;
}

/**
 * @brief Returns TRUE if Upper paints after Window and can occlude it.
 *        Children paint above their parent; later siblings paint above earlier ones.
 */
static
BOOL
IsStackedAbove(
    _In_ PX_WINDOW Upper,
    _In_ PX_WINDOW Window)
{
    PX_WINDOW A;
    PX_WINDOW B;
    PX_WINDOW Walk;
    INT DepthA = 0;
    INT DepthB = 0;

    if (Upper == Window)
        return FALSE;
    if (IsAncestor(Window, Upper))
        return TRUE;
    if (IsAncestor(Upper, Window))
        return FALSE;

    for (Walk = Upper; Walk; Walk = Walk->Parent)
        DepthA++;
    for (Walk = Window; Walk; Walk = Walk->Parent)
        DepthB++;

    A = Upper;
    B = Window;
    while (DepthA > DepthB)
    {
        A = A->Parent;
        DepthA--;
    }
    while (DepthB > DepthA)
    {
        B = B->Parent;
        DepthB--;
    }
    while (A->Parent != B->Parent)
    {
        A = A->Parent;
        B = B->Parent;
    }

    for (Walk = B->NextSibling; Walk; Walk = Walk->NextSibling)
    {
        if (Walk == A)
            return TRUE;
    }
    return FALSE;
}

/**
 * @brief Builds the visible region of a window minus every viewable window stacked above it.
 * @return Region owned by the caller.
 */
static
HRGN
WindowClipRegion(
    _In_ PX_WINDOW Window)
{
    RECT Visible;
    RECT OccluderRect;
    HRGN Region;
    HRGN Occluder;
    PX_WINDOW Other;
    INT AbsX;
    INT AbsY;

    WindowVisibleRect(Window, &Visible);
    Region = CreateRectRgnIndirect(&Visible);
    for (Other = g_AllWindows; Other; Other = Other->AllNext)
    {
        if (Other == Window || !IsWindowViewable(Other))
            continue;
        if (IsAncestor(Other, Window))
            continue;
        if (!IsStackedAbove(Other, Window))
            continue;

        WindowAbsolutePosition(Other, &AbsX, &AbsY);
        OccluderRect.left = AbsX - Other->BorderWidth;
        OccluderRect.top = AbsY - Other->BorderWidth;
        OccluderRect.right = AbsX + Other->Width + Other->BorderWidth;
        OccluderRect.bottom = AbsY + Other->Height + Other->BorderWidth;
        Occluder = CreateRectRgnIndirect(&OccluderRect);
        CombineRgn(Region, Region, Occluder, RGN_DIFF);
        DeleteObject(Occluder);
    }
    return Region;
}

/**
 * @brief Raises a window to the top of its siblings.
 */
static
VOID
RaiseXWindow(
    _In_ PX_WINDOW Window)
{
    PX_WINDOW Parent = Window->Parent;

    if (!Parent || Parent->LastChild == Window)
        return;

    TreeUnlink(Window);
    TreeLink(Parent, Window);
}

/* Event delivery */

/**
 * @brief Sends a 32-byte event to every client that selected Mask on Window.
 */
static
VOID
DeliverEvent(
    _In_ PX_WINDOW Window,
    _In_ DWORD Mask,
    _Inout_updates_bytes_(32) PBYTE Event)
{
    INT Index;
    INT Sent = 0;

    for (Index = 0; Index < Window->SelectorCount; Index++)
    {
        if (Window->Selectors[Index].Mask & Mask)
        {
            PX_CLIENT Client = Window->Selectors[Index].Client;

            Event[2] = (BYTE)(Client->Sequence & 0xFF);
            Event[3] = (BYTE)((Client->Sequence >> 8) & 0xFF);
            SendToClient(Client, Event, 32);
            Sent++;
        }
    }
    XTrace("psxx11: event %u win=%lx mask=%lx -> %d\n", Event[0], (ULONG)Window->Id, Mask, Sent);
}

/*
 * Atom table. Atoms are stable per name and unique across names, and InternAtom
 * honors only-if-exists so clients can probe for running services.
 */
static const PCSTR g_PredefinedAtoms[] =
{
    "",
    "PRIMARY", "SECONDARY", "ARC", "ATOM", "BITMAP", "CARDINAL", "COLORMAP",
    "CURSOR", "CUT_BUFFER0", "CUT_BUFFER1", "CUT_BUFFER2", "CUT_BUFFER3",
    "CUT_BUFFER4", "CUT_BUFFER5", "CUT_BUFFER6", "CUT_BUFFER7", "DRAWABLE",
    "FONT", "INTEGER", "PIXMAP", "POINT", "RECTANGLE", "RESOURCE_MANAGER",
    "RGB_COLOR_MAP", "RGB_BEST_MAP", "RGB_BLUE_MAP", "RGB_DEFAULT_MAP",
    "RGB_GRAY_MAP", "RGB_GREEN_MAP", "RGB_RED_MAP", "STRING", "VISUALID",
    "WINDOW", "WM_COMMAND", "WM_HINTS", "WM_CLIENT_MACHINE", "WM_ICON_NAME",
    "WM_ICON_SIZE", "WM_NAME", "WM_NORMAL_HINTS", "WM_SIZE_HINTS",
    "WM_ZOOM_HINTS", "MIN_SPACE", "NORM_SPACE", "MAX_SPACE", "END_SPACE",
    "SUPERSCRIPT_X", "SUPERSCRIPT_Y", "SUBSCRIPT_X", "SUBSCRIPT_Y",
    "UNDERLINE_POSITION", "UNDERLINE_THICKNESS", "STRIKEOUT_ASCENT",
    "STRIKEOUT_DESCENT", "ITALIC_ANGLE", "X_HEIGHT", "QUAD_WIDTH", "WEIGHT",
    "POINT_SIZE", "RESOLUTION", "COPYRIGHT", "NOTICE", "FONT_NAME",
    "FAMILY_NAME", "FULL_NAME", "CAP_HEIGHT", "WM_CLASS", "WM_TRANSIENT_FOR"
};
#define NUM_PREDEF_ATOMS 68     /* XA_PRIMARY (1) through XA_WM_TRANSIENT_FOR (68) */
#define MAX_DYN_ATOMS    512
#define ATOM_NAME_MAX    80
#define DYN_ATOM_BASE    100
static CHAR g_DynamicAtoms[MAX_DYN_ATOMS][ATOM_NAME_MAX];
static INT g_DynamicAtomCount;

static
BOOL
AtomNameEquals(
    _In_z_ PCSTR TableName,
    _In_reads_(Length) PCSTR Name,
    _In_ INT Length)
{
    INT Index;

    for (Index = 0; Index < Length; Index++)
    {
        if (TableName[Index] == '\0' || TableName[Index] != Name[Index])
            return FALSE;
    }
    return TableName[Length] == '\0';
}

static
XID
InternAtomByName(
    _In_reads_(Length) PCSTR Name,
    _In_ INT Length,
    _In_ BOOL OnlyIfExists)
{
    INT Index;

    if (Length <= 0 || Length >= ATOM_NAME_MAX)
        return 0;

    for (Index = 1; Index <= NUM_PREDEF_ATOMS; Index++)
    {
        if (AtomNameEquals(g_PredefinedAtoms[Index], Name, Length))
            return (XID)Index;
    }
    for (Index = 0; Index < g_DynamicAtomCount; Index++)
    {
        if (AtomNameEquals(g_DynamicAtoms[Index], Name, Length))
            return (XID)(DYN_ATOM_BASE + Index);
    }

    if (OnlyIfExists || g_DynamicAtomCount >= MAX_DYN_ATOMS)
        return 0;

    RtlCopyMemory(g_DynamicAtoms[g_DynamicAtomCount], Name, Length);
    g_DynamicAtoms[g_DynamicAtomCount][Length] = '\0';
    return (XID)(DYN_ATOM_BASE + g_DynamicAtomCount++);
}

static
PCSTR
GetAtomNameById(
    _In_ XID Atom)
{
    if (Atom >= 1 && Atom <= NUM_PREDEF_ATOMS)
        return g_PredefinedAtoms[Atom];
    if (Atom >= DYN_ATOM_BASE && Atom < (XID)(DYN_ATOM_BASE + g_DynamicAtomCount))
        return g_DynamicAtoms[Atom - DYN_ATOM_BASE];
    return NULL;
}

/* Selection ownership, enough for a window manager to claim WM_S<n> and verify it */
typedef struct _X_SELECTION
{
    XID Atom;
    XID Owner;
} X_SELECTION, *PX_SELECTION;

#define MAX_SELECTIONS 32
static X_SELECTION g_Selections[MAX_SELECTIONS];
static INT g_SelectionCount = 0;

static
VOID
SetSelectionOwnerAtom(
    _In_ XID Atom,
    _In_ XID Owner)
{
    INT Index;

    for (Index = 0; Index < g_SelectionCount; Index++)
    {
        if (g_Selections[Index].Atom == Atom)
        {
            g_Selections[Index].Owner = Owner;
            return;
        }
    }

    if (g_SelectionCount < MAX_SELECTIONS)
    {
        g_Selections[g_SelectionCount].Atom = Atom;
        g_Selections[g_SelectionCount].Owner = Owner;
        g_SelectionCount++;
    }
}

static
XID
GetSelectionOwnerAtom(
    _In_ XID Atom)
{
    INT Index;

    for (Index = 0; Index < g_SelectionCount; Index++)
    {
        if (g_Selections[Index].Atom == Atom)
            return g_Selections[Index].Owner;
    }
    return 0;
}

/**
 * @brief Finds the client other than Except that selected SubstructureRedirect on Window.
 */
static
PX_CLIENT
FindRedirectClient(
    _In_ PX_WINDOW Window,
    _In_opt_ PX_CLIENT Except)
{
    INT Index;

    for (Index = 0; Index < Window->SelectorCount; Index++)
    {
        if ((Window->Selectors[Index].Mask & SubstructureRedirectMask) &&
            Window->Selectors[Index].Client != Except)
        {
            return Window->Selectors[Index].Client;
        }
    }
    return NULL;
}

static
VOID
SendExpose(
    _In_ PX_WINDOW Window,
    _In_ INT X,
    _In_ INT Y,
    _In_ INT Width,
    _In_ INT Height)
{
    BYTE Event[32];

    RtlZeroMemory(Event, sizeof(Event));
    Event[0] = Expose;
    *(DWORD *)(Event + 4) = (DWORD)Window->Id;
    *(WORD *)(Event + 8) = (WORD)X;
    *(WORD *)(Event + 10) = (WORD)Y;
    *(WORD *)(Event + 12) = (WORD)Width;
    *(WORD *)(Event + 14) = (WORD)Height;
    DeliverEvent(Window, ExposureMask, Event);
}

/**
 * @brief Sends an event to SubstructureNotify selectors on the parent and StructureNotify on the window.
 */
static
VOID
NotifyStructure(
    _In_ PX_WINDOW Window,
    _Inout_updates_bytes_(32) PBYTE Event)
{
    if (Window->Parent)
        DeliverEvent(Window->Parent, SubstructureNotifyMask, Event);
    DeliverEvent(Window, StructureNotifyMask, Event);
}

static
VOID
SendMapNotify(
    _In_ PX_WINDOW Window)
{
    BYTE Event[32];

    RtlZeroMemory(Event, sizeof(Event));
    Event[0] = MapNotify;
    *(DWORD *)(Event + 4) = Window->Parent ? (DWORD)Window->Parent->Id : 0;
    *(DWORD *)(Event + 8) = (DWORD)Window->Id;
    Event[12] = (BYTE)Window->OverrideRedirect;
    NotifyStructure(Window, Event);
}

static
VOID
SendUnmapNotify(
    _In_ PX_WINDOW Window)
{
    BYTE Event[32];

    RtlZeroMemory(Event, sizeof(Event));
    Event[0] = UnmapNotify;
    *(DWORD *)(Event + 4) = Window->Parent ? (DWORD)Window->Parent->Id : 0;
    *(DWORD *)(Event + 8) = (DWORD)Window->Id;
    NotifyStructure(Window, Event);
}

static
VOID
SendConfigureNotify(
    _In_ PX_WINDOW Window)
{
    BYTE Event[32];
    INT AbsX;
    INT AbsY;

    RtlZeroMemory(Event, sizeof(Event));
    WindowAbsolutePosition(Window, &AbsX, &AbsY);
    Event[0] = ConfigureNotify;
    *(DWORD *)(Event + 4) = Window->Parent ? (DWORD)Window->Parent->Id : 0;
    *(DWORD *)(Event + 8) = (DWORD)Window->Id;
    *(DWORD *)(Event + 12) = 0;     /* above-sibling: None */
    *(WORD *)(Event + 16) = (WORD)Window->X;
    *(WORD *)(Event + 18) = (WORD)Window->Y;
    *(WORD *)(Event + 20) = (WORD)Window->Width;
    *(WORD *)(Event + 22) = (WORD)Window->Height;
    *(WORD *)(Event + 24) = (WORD)Window->BorderWidth;
    NotifyStructure(Window, Event);
}

static
VOID
SendCreateNotify(
    _In_ PX_WINDOW Window)
{
    BYTE Event[32];

    RtlZeroMemory(Event, sizeof(Event));
    Event[0] = CreateNotify;
    *(DWORD *)(Event + 4) = Window->Parent ? (DWORD)Window->Parent->Id : 0;
    *(DWORD *)(Event + 8) = (DWORD)Window->Id;
    *(WORD *)(Event + 12) = (WORD)Window->X;
    *(WORD *)(Event + 14) = (WORD)Window->Y;
    *(WORD *)(Event + 16) = (WORD)Window->Width;
    *(WORD *)(Event + 18) = (WORD)Window->Height;
    *(WORD *)(Event + 20) = (WORD)Window->BorderWidth;
    Event[22] = (BYTE)Window->OverrideRedirect;
    if (Window->Parent)
        DeliverEvent(Window->Parent, SubstructureNotifyMask, Event);
}

static
VOID
SendDestroyNotify(
    _In_ PX_WINDOW Window)
{
    BYTE Event[32];

    RtlZeroMemory(Event, sizeof(Event));
    Event[0] = DestroyNotify;
    *(DWORD *)(Event + 4) = Window->Parent ? (DWORD)Window->Parent->Id : 0;
    *(DWORD *)(Event + 8) = (DWORD)Window->Id;
    NotifyStructure(Window, Event);
}

static
VOID
SendReparentNotify(
    _In_ PX_WINDOW Window)
{
    BYTE Event[32];

    RtlZeroMemory(Event, sizeof(Event));
    Event[0] = ReparentNotify;
    *(DWORD *)(Event + 4) = (DWORD)Window->Id;
    *(DWORD *)(Event + 8) = (DWORD)Window->Id;
    *(DWORD *)(Event + 12) = Window->Parent ? (DWORD)Window->Parent->Id : 0;
    *(WORD *)(Event + 16) = (WORD)Window->X;
    *(WORD *)(Event + 18) = (WORD)Window->Y;
    Event[20] = (BYTE)Window->OverrideRedirect;
    NotifyStructure(Window, Event);
}

/* Painting */

static
VOID
FillScreenRect(
    _In_ PRECT Rect,
    _In_ DWORD Pixel)
{
    HBRUSH Brush;

    if (Rect->right <= Rect->left || Rect->bottom <= Rect->top)
        return;

    Brush = CreateSolidBrush(PixelToColor(Pixel));
    FillRect(g_ScreenDc, Rect, Brush);
    DeleteObject(Brush);
}

/**
 * @brief Fills a screen rect with a window's effective background.
 *        None and ParentRelative use the nearest ancestor with a background;
 *        a background pixmap tiles from the owning window's origin.
 */
static
VOID
FillBackgroundRect(
    _In_ PX_WINDOW Window,
    _In_ PRECT Rect)
{
    PX_WINDOW Source = Window;

    if (Rect->right <= Rect->left || Rect->bottom <= Rect->top)
        return;

    while (Source && !Source->HasBackground && Source->BackgroundPixmap <= 1)
        Source = Source->Parent;

    if (Source && Source->BackgroundPixmap > 1)
    {
        PX_PIXMAP Pixmap = FindPixmap(Source->BackgroundPixmap);

        if (Pixmap && Pixmap->Bitmap)
        {
            HBRUSH Brush = CreatePatternBrush(Pixmap->Bitmap);

            if (Brush)
            {
                INT AbsX;
                INT AbsY;

                WindowAbsolutePosition(Source, &AbsX, &AbsY);
                SetBrushOrgEx(g_ScreenDc, AbsX, AbsY, NULL);
                FillRect(g_ScreenDc, Rect, Brush);
                SetBrushOrgEx(g_ScreenDc, 0, 0, NULL);
                DeleteObject(Brush);
                return;
            }
        }
    }

    FillScreenRect(Rect, (Source && Source->HasBackground) ? Source->BackgroundPixel : 0x00808080);
}

/**
 * @brief Paints a window's border and background into its visible rect.
 */
static
VOID
PaintWindowBackground(
    _In_ PX_WINDOW Window)
{
    RECT Visible;

    if (!IsWindowViewable(Window))
        return;

    WindowVisibleRect(Window, &Visible);
    if (Window->BorderWidth > 0 && Window->Parent)
    {
        INT AbsX;
        INT AbsY;
        RECT Border;

        WindowAbsolutePosition(Window, &AbsX, &AbsY);
        Border.left = AbsX - Window->BorderWidth;
        Border.top = AbsY - Window->BorderWidth;
        Border.right = AbsX + Window->Width + Window->BorderWidth;
        Border.bottom = AbsY + Window->Height + Window->BorderWidth;
        FillScreenRect(&Border, Window->BorderPixel);
    }
    FillBackgroundRect(Window, &Visible);
}

/**
 * @brief Paints a window subtree back to front, then asks clients to redraw with Expose.
 */
static
VOID
PaintTree(
    _In_ PX_WINDOW Window)
{
    PX_WINDOW Child;

    if (!Window->Mapped)
        return;

    PaintWindowBackground(Window);
    for (Child = Window->FirstChild; Child; Child = Child->NextSibling)
        PaintTree(Child);

    if (Window != g_Root)
        SendExpose(Window, 0, 0, Window->Width, Window->Height);
}

static
VOID
RepaintScreen(VOID)
{
    PaintTree(g_Root);
    InterlockedExchange(&g_Dirty, 1);
}

/* Drawing targets: a drawable resolved to a DC, origin offset and clip region */

typedef struct _X_TARGET
{
    HDC Dc;
    INT OffsetX;
    INT OffsetY;
    HRGN Clip;
    BOOL Valid;
} X_TARGET, *PX_TARGET;

static
VOID
ResolveDrawable(
    _In_ XID Id,
    _Out_ PX_TARGET Target)
{
    PX_WINDOW Window;
    PX_PIXMAP Pixmap;

    Target->Valid = FALSE;
    Target->Clip = NULL;

    if ((Window = FindWindowById(Id)) != NULL)
    {
        INT AbsX;
        INT AbsY;

        /* Drawing to an unmapped window is dropped */
        if (!IsWindowViewable(Window))
        {
            static INT DropLogs;

            if (DropLogs < 6)
            {
                DropLogs++;
                XWmLog("drop %lx", (ULONG)Id);
            }
            return;
        }

        WindowAbsolutePosition(Window, &AbsX, &AbsY);
        Target->Clip = WindowClipRegion(Window);
        Target->Dc = g_ScreenDc;
        Target->OffsetX = AbsX;
        Target->OffsetY = AbsY;
        Target->Valid = TRUE;
    }
    else if ((Pixmap = FindPixmap(Id)) != NULL)
    {
        Target->Dc = Pixmap->Dc;
        Target->OffsetX = 0;
        Target->OffsetY = 0;
        Target->Clip = CreateRectRgn(0, 0, Pixmap->Width, Pixmap->Height);
        Target->Valid = TRUE;
    }
}

static
HRGN
BeginTarget(
    _In_ PX_TARGET Target)
{
    SelectClipRgn(Target->Dc, Target->Clip);
    return Target->Clip;
}

/**
 * @brief Ends a draw and resets the DC state that ApplyGcState may have changed.
 */
static
VOID
EndTarget(
    _In_ PX_TARGET Target,
    _In_opt_ HRGN Region)
{
    UNREFERENCED_PARAMETER(Region);

    SelectClipRgn(Target->Dc, NULL);
    SetROP2(Target->Dc, R2_COPYPEN);
    SetPolyFillMode(Target->Dc, ALTERNATE);
    SetBrushOrgEx(Target->Dc, 0, 0, NULL);
    DeleteObject(Target->Clip);
    Target->Clip = NULL;
    InterlockedExchange(&g_Dirty, 1);
}

/* GC to GDI mapping */

/**
 * @brief Maps an X raster function to a GDI binary raster op for pen and brush drawing.
 */
static
INT
GxToRop2(
    _In_ DWORD Function)
{
    static const INT Rop2Table[16] =
    {
        R2_BLACK,       R2_MASKPEN,     R2_MASKPENNOT, R2_COPYPEN,
        R2_MASKNOTPEN,  R2_NOP,         R2_XORPEN,     R2_MERGEPEN,
        R2_NOTMERGEPEN, R2_NOTXORPEN,   R2_NOT,        R2_MERGEPENNOT,
        R2_NOTCOPYPEN,  R2_MERGENOTPEN, R2_NOTMASKPEN, R2_WHITE
    };

    return Rop2Table[Function & 15];
}

/**
 * @brief Maps an X raster function to a pattern rop3 for rect fills, which ignore ROP2.
 */
static
DWORD
GxToPatRop3(
    _In_ DWORD Function)
{
    switch (Function & 15)
    {
        case GXclear:
            return BLACKNESS;
        case 0x1:
            return 0x00A000C9;  /* GXand: D & P */
        case GXxor:
            return PATINVERT;
        case 0x7:
            return 0x00FA0089;  /* GXor: D | P */
        case GXinvert:
            return DSTINVERT;
        case 0xF:
            return WHITENESS;
        default:
            return PATCOPY;
    }
}

/**
 * @brief Maps an X raster function to a source rop3 for CopyArea and CopyPlane.
 */
static
DWORD
GxToSrcRop3(
    _In_ DWORD Function)
{
    switch (Function & 15)
    {
        case GXclear:
            return BLACKNESS;
        case 0x1:
            return SRCAND;
        case GXxor:
            return SRCINVERT;
        case 0x7:
            return SRCPAINT;
        case 0xC:
            return NOTSRCCOPY;
        case GXinvert:
            return DSTINVERT;
        case 0xF:
            return WHITENESS;
        default:
            return SRCCOPY;
    }
}

/**
 * @brief Integer sine scaled by 1024 for Degrees in [0, 90].
 */
static
INT
SineTable1024(
    _In_ INT Degrees)
{
    static const SHORT SineTable[91] =
    {
        0, 18, 36, 54, 71, 89, 107, 125, 143, 160, 178, 195, 213, 230, 248, 265, 282, 299, 316, 333,
        350, 367, 384, 400, 416, 433, 449, 465, 481, 496, 512, 527, 543, 558, 573, 587, 602, 616,
        630, 644, 658, 672, 685, 698, 711, 724, 737, 749, 761, 773, 784, 796, 807, 818, 828, 839,
        849, 859, 868, 878, 887, 896, 904, 912, 920, 928, 935, 943, 949, 956, 962, 968, 974, 979,
        984, 989, 994, 998, 1002, 1005, 1008, 1011, 1014, 1016, 1018, 1020, 1022, 1023, 1023, 1024, 1024
    };

    return SineTable[Degrees < 0 ? 0 : (Degrees > 90 ? 90 : Degrees)];
}

/**
 * @brief Integer sine scaled by 1024 for any angle in degrees.
 */
static
INT
Sine1024(
    _In_ INT Degrees)
{
    Degrees %= 360;
    if (Degrees < 0)
        Degrees += 360;

    if (Degrees <= 90)
        return SineTable1024(Degrees);
    if (Degrees <= 180)
        return SineTable1024(180 - Degrees);
    if (Degrees <= 270)
        return -SineTable1024(Degrees - 180);
    return -SineTable1024(360 - Degrees);
}

static
INT
Cosine1024(
    _In_ INT Degrees)
{
    return Sine1024(Degrees + 90);
}

/**
 * @brief Creates a brush for the GC fill style. Stippled fills rely on ApplyGcState
 *        swapping the DC colors, since GDI maps mono 1-bits to the background color.
 */
static
HBRUSH
CreateGcBrush(
    _In_opt_ PX_GCONTEXT Gc)
{
    if (Gc)
    {
        PX_PIXMAP Pixmap = NULL;

        if (Gc->FillStyle == 1 && Gc->Tile)
            Pixmap = FindPixmap(Gc->Tile);
        else if ((Gc->FillStyle == 2 || Gc->FillStyle == 3) && Gc->Stipple)
            Pixmap = FindPixmap(Gc->Stipple);

        if (Pixmap && Pixmap->Bitmap)
        {
            HBRUSH Brush = CreatePatternBrush(Pixmap->Bitmap);

            if (Brush)
                return Brush;
        }
    }
    return CreateSolidBrush(PixelToColor(Gc ? Gc->Foreground : 0));
}

/**
 * @brief Creates a pen for the GC line width, style, cap and join. Dashes use PS_DASH.
 */
static
HPEN
CreateGcPen(
    _In_opt_ PX_GCONTEXT Gc)
{
    DWORD Width = (Gc && Gc->LineWidth) ? Gc->LineWidth : 1;
    DWORD Style;
    LOGBRUSH LogBrush;
    HPEN Pen;

    if (!Gc || (Gc->LineStyle == 0 && Gc->CapStyle <= 1 && Gc->JoinStyle == 0))
        return CreatePen(PS_SOLID, (INT)Width, PixelToColor(Gc ? Gc->Foreground : 0));

    Style = PS_GEOMETRIC | (Gc->LineStyle ? PS_DASH : PS_SOLID);

    if (Gc->CapStyle == 2)
        Style |= PS_ENDCAP_ROUND;
    else if (Gc->CapStyle == 3)
        Style |= PS_ENDCAP_SQUARE;
    else
        Style |= PS_ENDCAP_FLAT;

    if (Gc->JoinStyle == 1)
        Style |= PS_JOIN_ROUND;
    else if (Gc->JoinStyle == 2)
        Style |= PS_JOIN_BEVEL;
    else
        Style |= PS_JOIN_MITER;

    LogBrush.lbStyle = BS_SOLID;
    LogBrush.lbColor = PixelToColor(Gc->Foreground);
    LogBrush.lbHatch = 0;
    Pen = ExtCreatePen(Style, Width, &LogBrush, 0, NULL);
    return Pen ? Pen : CreatePen(PS_SOLID, (INT)Width, PixelToColor(Gc->Foreground));
}

/**
 * @brief Installs per-GC DC state after BeginTarget: raster op, fill rule, pattern origin
 *        and colors, and the GC clip rectangles. EndTarget resets all of it.
 */
static
VOID
ApplyGcState(
    _In_ PX_TARGET Target,
    _In_opt_ PX_GCONTEXT Gc)
{
    SetROP2(Target->Dc, GxToRop2(Gc ? Gc->Function : GXcopy));
    if (!Gc)
        return;

    /* IncludeInferiors: draw over child windows by dropping the occlusion clip */
    if (Gc->SubwindowMode == 1)
        SelectClipRgn(Target->Dc, NULL);

    SetPolyFillMode(Target->Dc, (Gc->FillRule == 1) ? WINDING : ALTERNATE);
    SetBrushOrgEx(Target->Dc,
                  Target->OffsetX + Gc->TileStipXOrigin,
                  Target->OffsetY + Gc->TileStipYOrigin,
                  NULL);

    if (Gc->FillStyle == 2 || Gc->FillStyle == 3)
    {
        SetTextColor(Target->Dc, PixelToColor(Gc->Background));
        SetBkColor(Target->Dc, PixelToColor(Gc->Foreground));
    }

    if (Gc->ClipRectCount > 0 && Gc->ClipRects)
    {
        HRGN Combined = CreateRectRgn(0, 0, 0, 0);
        INT Index;

        for (Index = 0; Index < Gc->ClipRectCount; Index++)
        {
            HRGN Single = CreateRectRgn(Target->OffsetX + Gc->ClipXOrigin + Gc->ClipRects[Index].left,
                                        Target->OffsetY + Gc->ClipYOrigin + Gc->ClipRects[Index].top,
                                        Target->OffsetX + Gc->ClipXOrigin + Gc->ClipRects[Index].right,
                                        Target->OffsetY + Gc->ClipYOrigin + Gc->ClipRects[Index].bottom);

            CombineRgn(Combined, Combined, Single, RGN_OR);
            DeleteObject(Single);
        }
        ExtSelectClipRgn(Target->Dc, Combined, RGN_AND);
        DeleteObject(Combined);
    }
}

static
VOID
DrawFillRects(
    _In_ PX_TARGET Target,
    _In_opt_ PX_GCONTEXT Gc,
    _In_reads_(Count) PCX_RECT Rects,
    _In_ DWORD Count)
{
    HBRUSH Brush = CreateGcBrush(Gc);
    DWORD Rop3 = GxToPatRop3(Gc ? Gc->Function : GXcopy);
    HRGN Region = BeginTarget(Target);
    HBRUSH OldBrush;
    DWORD Index;

    ApplyGcState(Target, Gc);
    OldBrush = (HBRUSH)SelectObject(Target->Dc, Brush);

    /* GXnoop (5) draws nothing */
    if (!(Gc && Gc->Function == 5))
    {
        for (Index = 0; Index < Count; Index++)
        {
            PatBlt(Target->Dc,
                   Target->OffsetX + Rects[Index].x,
                   Target->OffsetY + Rects[Index].y,
                   Rects[Index].width,
                   Rects[Index].height,
                   Rop3);
        }
    }

    SelectObject(Target->Dc, OldBrush);
    EndTarget(Target, Region);
    DeleteObject(Brush);
}

static
VOID
DrawFrameRects(
    _In_ PX_TARGET Target,
    _In_opt_ PX_GCONTEXT Gc,
    _In_reads_(Count) PCX_RECT Rects,
    _In_ DWORD Count)
{
    HPEN Pen = CreateGcPen(Gc);
    HRGN Region = BeginTarget(Target);
    HPEN OldPen;
    HBRUSH OldBrush;
    DWORD Index;

    ApplyGcState(Target, Gc);
    OldPen = (HPEN)SelectObject(Target->Dc, Pen);
    OldBrush = (HBRUSH)SelectObject(Target->Dc, GetStockObject(NULL_BRUSH));
    for (Index = 0; Index < Count; Index++)
    {
        Rectangle(Target->Dc,
                  Target->OffsetX + Rects[Index].x,
                  Target->OffsetY + Rects[Index].y,
                  Target->OffsetX + Rects[Index].x + Rects[Index].width + 1,
                  Target->OffsetY + Rects[Index].y + Rects[Index].height + 1);
    }
    SelectObject(Target->Dc, OldBrush);
    SelectObject(Target->Dc, OldPen);
    EndTarget(Target, Region);
    DeleteObject(Pen);
}

static
VOID
DrawSegments(
    _In_ PX_TARGET Target,
    _In_opt_ PX_GCONTEXT Gc,
    _In_reads_(Count) PCX_SEGMENT Segments,
    _In_ DWORD Count)
{
    HPEN Pen = CreateGcPen(Gc);
    HRGN Region = BeginTarget(Target);
    HPEN OldPen;
    DWORD Index;

    ApplyGcState(Target, Gc);
    OldPen = (HPEN)SelectObject(Target->Dc, Pen);
    for (Index = 0; Index < Count; Index++)
    {
        MoveToEx(Target->Dc,
                 Target->OffsetX + Segments[Index].x1,
                 Target->OffsetY + Segments[Index].y1,
                 NULL);
        LineTo(Target->Dc, Target->OffsetX + Segments[Index].x2, Target->OffsetY + Segments[Index].y2);

        /* X includes the end point, GDI does not */
        SetPixelV(Target->Dc,
                  Target->OffsetX + Segments[Index].x2,
                  Target->OffsetY + Segments[Index].y2,
                  PixelToColor(Gc ? Gc->Foreground : 0));
    }
    SelectObject(Target->Dc, OldPen);
    EndTarget(Target, Region);
    DeleteObject(Pen);
}

static
VOID
DrawPolyline(
    _In_ PX_TARGET Target,
    _In_opt_ PX_GCONTEXT Gc,
    _In_reads_(Count) PCX_POINT Points,
    _In_ DWORD Count,
    _In_ BYTE CoordMode)
{
    HPEN Pen;
    HPEN OldPen;
    HRGN Region;
    DWORD Index;
    INT CurrentX;
    INT CurrentY;

    if (Count == 0)
        return;

    Pen = CreateGcPen(Gc);
    Region = BeginTarget(Target);
    ApplyGcState(Target, Gc);
    OldPen = (HPEN)SelectObject(Target->Dc, Pen);
    CurrentX = Target->OffsetX + Points[0].x;
    CurrentY = Target->OffsetY + Points[0].y;
    MoveToEx(Target->Dc, CurrentX, CurrentY, NULL);
    for (Index = 1; Index < Count; Index++)
    {
        if (CoordMode)
        {
            CurrentX += Points[Index].x;
            CurrentY += Points[Index].y;
        }
        else
        {
            CurrentX = Target->OffsetX + Points[Index].x;
            CurrentY = Target->OffsetY + Points[Index].y;
        }
        LineTo(Target->Dc, CurrentX, CurrentY);
    }
    SelectObject(Target->Dc, OldPen);
    EndTarget(Target, Region);
    DeleteObject(Pen);
}

static
VOID
DrawPoints(
    _In_ PX_TARGET Target,
    _In_opt_ PX_GCONTEXT Gc,
    _In_reads_(Count) PCX_POINT Points,
    _In_ DWORD Count,
    _In_ BYTE CoordMode)
{
    HRGN Region;
    COLORREF Color = PixelToColor(Gc ? Gc->Foreground : 0);
    DWORD Index;
    INT CurrentX = 0;
    INT CurrentY = 0;

    if (Count == 0)
        return;

    Region = BeginTarget(Target);
    for (Index = 0; Index < Count; Index++)
    {
        if (CoordMode && Index)
        {
            CurrentX += Points[Index].x;
            CurrentY += Points[Index].y;
        }
        else
        {
            CurrentX = Target->OffsetX + Points[Index].x;
            CurrentY = Target->OffsetY + Points[Index].y;
        }
        SetPixelV(Target->Dc, CurrentX, CurrentY, Color);
    }
    EndTarget(Target, Region);
}

static
VOID
DrawFillPoly(
    _In_ PX_TARGET Target,
    _In_opt_ PX_GCONTEXT Gc,
    _In_reads_(Count) PCX_POINT Points,
    _In_ DWORD Count)
{
    PPOINT GdiPoints;
    HBRUSH Brush;
    HBRUSH OldBrush;
    HPEN OldPen;
    HRGN Region;
    DWORD Index;

    if (Count < 2)
        return;

    GdiPoints = (PPOINT)HeapAlloc(GetProcessHeap(), 0, Count * sizeof(*GdiPoints));
    if (!GdiPoints)
        return;

    for (Index = 0; Index < Count; Index++)
    {
        GdiPoints[Index].x = Target->OffsetX + Points[Index].x;
        GdiPoints[Index].y = Target->OffsetY + Points[Index].y;
    }

    Brush = CreateGcBrush(Gc);
    Region = BeginTarget(Target);
    ApplyGcState(Target, Gc);
    OldBrush = (HBRUSH)SelectObject(Target->Dc, Brush);
    OldPen = (HPEN)SelectObject(Target->Dc, GetStockObject(NULL_PEN));
    Polygon(Target->Dc, GdiPoints, (INT)Count);
    SelectObject(Target->Dc, OldPen);
    SelectObject(Target->Dc, OldBrush);
    EndTarget(Target, Region);
    DeleteObject(Brush);
    HeapFree(GetProcessHeap(), 0, GdiPoints);
}

/**
 * @brief Converts X arc angles (1/64 degree, counterclockwise from 3 o'clock, angle2 is
 *        the sweep) to the radial end points GDI Arc, Chord and Pie expect.
 */
static
VOID
ArcEndpoints(
    _In_ PCX_ARC Arc,
    _In_ INT OffsetX,
    _In_ INT OffsetY,
    _Out_ PPOINT Start,
    _Out_ PPOINT End)
{
    /* Doubled center avoids truncation */
    INT CenterX2 = 2 * OffsetX + 2 * Arc->x + Arc->width;
    INT CenterY2 = 2 * OffsetY + 2 * Arc->y + Arc->height;
    INT StartDegrees = Arc->angle1 / 64;
    INT EndDegrees = (Arc->angle1 + Arc->angle2) / 64;

    /* Y grows downward, so the sine term is subtracted */
    Start->x = (CenterX2 + (INT)Arc->width * Cosine1024(StartDegrees) / 1024) / 2;
    Start->y = (CenterY2 - (INT)Arc->height * Sine1024(StartDegrees) / 1024) / 2;
    End->x = (CenterX2 + (INT)Arc->width * Cosine1024(EndDegrees) / 1024) / 2;
    End->y = (CenterY2 - (INT)Arc->height * Sine1024(EndDegrees) / 1024) / 2;
}

static
VOID
DrawFillArcs(
    _In_ PX_TARGET Target,
    _In_opt_ PX_GCONTEXT Gc,
    _In_reads_(Count) PCX_ARC Arcs,
    _In_ DWORD Count)
{
    HBRUSH Brush;
    HBRUSH OldBrush;
    HPEN OldPen;
    HRGN Region;
    DWORD Index;
    BOOL PieSlice = !Gc || Gc->ArcMode == 1;

    Brush = CreateGcBrush(Gc);
    Region = BeginTarget(Target);
    ApplyGcState(Target, Gc);
    OldBrush = (HBRUSH)SelectObject(Target->Dc, Brush);
    OldPen = (HPEN)SelectObject(Target->Dc, GetStockObject(NULL_PEN));
    for (Index = 0; Index < Count; Index++)
    {
        INT Left = Target->OffsetX + Arcs[Index].x;
        INT Top = Target->OffsetY + Arcs[Index].y;
        INT Right = Left + Arcs[Index].width + 1;
        INT Bottom = Top + Arcs[Index].height + 1;

        if ((INT)Arcs[Index].angle2 >= 360 * 64 || (INT)Arcs[Index].angle2 <= -360 * 64)
        {
            Ellipse(Target->Dc, Left, Top, Right, Bottom);
        }
        else
        {
            POINT Start;
            POINT End;

            ArcEndpoints(&Arcs[Index], Target->OffsetX, Target->OffsetY, &Start, &End);
            if ((INT)Arcs[Index].angle2 < 0)
            {
                POINT Swap = Start;

                Start = End;
                End = Swap;
            }

            if (PieSlice)
                Pie(Target->Dc, Left, Top, Right, Bottom, Start.x, Start.y, End.x, End.y);
            else
                Chord(Target->Dc, Left, Top, Right, Bottom, Start.x, Start.y, End.x, End.y);
        }
    }
    SelectObject(Target->Dc, OldPen);
    SelectObject(Target->Dc, OldBrush);
    EndTarget(Target, Region);
    DeleteObject(Brush);
}

static
VOID
DrawArcs(
    _In_ PX_TARGET Target,
    _In_opt_ PX_GCONTEXT Gc,
    _In_reads_(Count) PCX_ARC Arcs,
    _In_ DWORD Count)
{
    HPEN Pen = CreateGcPen(Gc);
    HRGN Region = BeginTarget(Target);
    HPEN OldPen;
    DWORD Index;

    ApplyGcState(Target, Gc);
    OldPen = (HPEN)SelectObject(Target->Dc, Pen);
    for (Index = 0; Index < Count; Index++)
    {
        INT Left = Target->OffsetX + Arcs[Index].x;
        INT Top = Target->OffsetY + Arcs[Index].y;
        INT Right = Left + Arcs[Index].width + 1;
        INT Bottom = Top + Arcs[Index].height + 1;

        if ((INT)Arcs[Index].angle2 >= 360 * 64 || (INT)Arcs[Index].angle2 <= -360 * 64)
        {
            HBRUSH OldBrush = (HBRUSH)SelectObject(Target->Dc, GetStockObject(NULL_BRUSH));

            Ellipse(Target->Dc, Left, Top, Right, Bottom);
            SelectObject(Target->Dc, OldBrush);
        }
        else
        {
            POINT Start;
            POINT End;

            ArcEndpoints(&Arcs[Index], Target->OffsetX, Target->OffsetY, &Start, &End);
            if ((INT)Arcs[Index].angle2 < 0)
            {
                POINT Swap = Start;

                Start = End;
                End = Swap;
            }
            Arc(Target->Dc, Left, Top, Right, Bottom, Start.x, Start.y, End.x, End.y);
        }
    }
    SelectObject(Target->Dc, OldPen);
    EndTarget(Target, Region);
    DeleteObject(Pen);
}

/**
 * @brief Draws text at a baseline. Opaque fills the background box (ImageText);
 *        GXxor XORs a glyph mask onto the destination.
 */
static
VOID
DrawTextGeneric(
    _In_ PX_TARGET Target,
    _In_opt_ PX_GCONTEXT Gc,
    _In_ SHORT X,
    _In_ SHORT Y,
    _In_reads_(Length) PCSTR String,
    _In_ INT Length,
    _In_ BOOL Opaque)
{
    INT DrawX = Target->OffsetX + X;
    INT DrawY = Target->OffsetY + Y - g_FontAscent;
    HRGN Region;

    if (Length <= 0)
        return;

    if (Gc && Gc->Function == GXxor)
    {
        SIZE Size;

        Region = BeginTarget(Target);
        ApplyGcState(Target, Gc);

        /* The blit below applies its own rop */
        SetROP2(Target->Dc, R2_COPYPEN);
        if (GetTextExtentPoint32A(Target->Dc, String, Length, &Size) && Size.cx > 0 && Size.cy > 0)
        {
            HDC MemoryDc = CreateCompatibleDC(Target->Dc);
            HBITMAP Bitmap = CreateCompatibleBitmap(Target->Dc, Size.cx, Size.cy);
            HBITMAP OldBitmap = (HBITMAP)SelectObject(MemoryDc, Bitmap);

            /* White glyphs on black, then XOR onto the destination */
            SelectObject(MemoryDc, GetStockObject(ANSI_FIXED_FONT));
            PatBlt(MemoryDc, 0, 0, Size.cx, Size.cy, BLACKNESS);
            SetTextColor(MemoryDc, RGB(255, 255, 255));
            SetBkMode(MemoryDc, TRANSPARENT);
            TextOutA(MemoryDc, 0, 0, String, Length);
            BitBlt(Target->Dc, DrawX, DrawY, Size.cx, Size.cy, MemoryDc, 0, 0, SRCINVERT);
            SelectObject(MemoryDc, OldBitmap);
            DeleteObject(Bitmap);
            DeleteDC(MemoryDc);
        }
        EndTarget(Target, Region);
    }
    else
    {
        Region = BeginTarget(Target);
        ApplyGcState(Target, Gc);
        SetROP2(Target->Dc, R2_COPYPEN);
        SetTextColor(Target->Dc, PixelToColor(Gc ? Gc->Foreground : 0));
        SetBkColor(Target->Dc, PixelToColor(Gc ? Gc->Background : WHITE_PIXEL));
        SetBkMode(Target->Dc, Opaque ? OPAQUE : TRANSPARENT);
        TextOutA(Target->Dc, DrawX, DrawY, String, Length);
        EndTarget(Target, Region);
    }
}

static
VOID
DrawImageText(
    _In_ PX_TARGET Target,
    _In_opt_ PX_GCONTEXT Gc,
    _In_ SHORT X,
    _In_ SHORT Y,
    _In_reads_(Length) PCSTR String,
    _In_ INT Length)
{
    DrawTextGeneric(Target, Gc, X, Y, String, Length, TRUE);
}

/**
 * @brief Converts depth-1 X image rows (LSB first, 32-bit scanline pad) to a GDI
 *        mono bitmap (MSB first, WORD aligned rows).
 * @return Bitmap owned by the caller, or NULL.
 */
static
HBITMAP
MonoBitmapFromXRows(
    _In_ WORD Width,
    _In_ WORD Height,
    _In_ BYTE LeftPad,
    _In_reads_bytes_(DataLength) const BYTE *Data,
    _In_ DWORD DataLength)
{
    DWORD SourceStride = (((DWORD)Width + LeftPad + 31) >> 5) << 2;
    DWORD DestStride = ((((DWORD)Width + 15) >> 4) << 1);
    PBYTE Buffer;
    HBITMAP Bitmap;
    DWORD Row;
    DWORD Column;

    if (Width == 0 || Height == 0 || SourceStride * Height > DataLength)
        return NULL;

    Buffer = (PBYTE)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, DestStride * Height);
    if (!Buffer)
        return NULL;

    for (Row = 0; Row < Height; Row++)
    {
        const BYTE *Source = Data + Row * SourceStride;
        PBYTE Dest = Buffer + Row * DestStride;

        for (Column = 0; Column < Width; Column++)
        {
            DWORD SourceBit = Column + LeftPad;

            if ((Source[SourceBit >> 3] >> (SourceBit & 7)) & 1)
                Dest[Column >> 3] |= (BYTE)(0x80 >> (Column & 7));
        }
    }

    Bitmap = CreateBitmap(Width, Height, 1, 1, Buffer);
    HeapFree(GetProcessHeap(), 0, Buffer);
    return Bitmap;
}

/**
 * @brief Blits a mono bitmap with X foreground/background semantics. GDI maps mono
 *        1-bits to the background color, so the DC colors are swapped.
 */
static
VOID
BlitMono(
    _In_ PX_TARGET Target,
    _In_opt_ PX_GCONTEXT Gc,
    _In_ HDC MonoDc,
    _In_ INT SourceX,
    _In_ INT SourceY,
    _In_ INT Width,
    _In_ INT Height,
    _In_ INT DestX,
    _In_ INT DestY,
    _In_ DWORD Rop3)
{
    COLORREF OldText = SetTextColor(Target->Dc, PixelToColor(Gc ? Gc->Background : WHITE_PIXEL));
    COLORREF OldBack = SetBkColor(Target->Dc, PixelToColor(Gc ? Gc->Foreground : 0));

    BitBlt(Target->Dc,
           Target->OffsetX + DestX,
           Target->OffsetY + DestY,
           Width,
           Height,
           MonoDc,
           SourceX,
           SourceY,
           Rop3);
    SetTextColor(Target->Dc, OldText);
    SetBkColor(Target->Dc, OldBack);
}

/**
 * @brief PutImage. Depth 24/32 ZPixmap is copied as 32bpp rows; XYBitmap and depth 1
 *        are converted to mono and drawn with the GC colors.
 */
static
VOID
DrawPutImage(
    _In_ PX_TARGET Target,
    _In_opt_ PX_GCONTEXT Gc,
    _In_ BYTE Format,
    _In_ SHORT DestX,
    _In_ SHORT DestY,
    _In_ WORD Width,
    _In_ WORD Height,
    _In_ BYTE LeftPad,
    _In_ BYTE Depth,
    _In_reads_bytes_(DataLength) const BYTE *Data,
    _In_ DWORD DataLength)
{
    HRGN Region;

    if (Width == 0 || Height == 0)
        return;

    if (Format == 0 || Depth == 1)
    {
        HBITMAP Bitmap = MonoBitmapFromXRows(Width, Height, LeftPad, Data, DataLength);
        HDC MemoryDc;

        if (!Bitmap)
            return;

        MemoryDc = CreateCompatibleDC(Target->Dc);
        SelectObject(MemoryDc, Bitmap);
        Region = BeginTarget(Target);
        BlitMono(Target, Gc, MemoryDc, 0, 0, Width, Height, DestX, DestY, SRCCOPY);
        EndTarget(Target, Region);
        DeleteDC(MemoryDc);
        DeleteObject(Bitmap);
    }
    else if (Depth == 24 || Depth == 32)
    {
        BITMAPINFO Bmi;

        if ((DWORD)Width * Height * 4 > DataLength)
            return;

        RtlZeroMemory(&Bmi, sizeof(Bmi));
        Bmi.bmiHeader.biSize = sizeof(Bmi.bmiHeader);
        Bmi.bmiHeader.biWidth = Width;
        Bmi.bmiHeader.biHeight = -(LONG)Height;
        Bmi.bmiHeader.biPlanes = 1;
        Bmi.bmiHeader.biBitCount = 32;
        Bmi.bmiHeader.biCompression = BI_RGB;
        Region = BeginTarget(Target);
        SetDIBitsToDevice(Target->Dc,
                          Target->OffsetX + DestX,
                          Target->OffsetY + DestY,
                          Width,
                          Height,
                          0,
                          0,
                          0,
                          Height,
                          Data,
                          &Bmi,
                          DIB_RGB_COLORS);
        EndTarget(Target, Region);
    }
}

/**
 * @brief CopyPlane. A mono source blits through the GC colors; a color source is
 *        thresholded by GDI's color to mono conversion.
 */
static
VOID
DoCopyPlane(
    _In_ XID SourceId,
    _In_ XID DestId,
    _In_opt_ PX_GCONTEXT Gc,
    _In_ INT SourceX,
    _In_ INT SourceY,
    _In_ INT Width,
    _In_ INT Height,
    _In_ INT DestX,
    _In_ INT DestY)
{
    X_TARGET Source;
    X_TARGET Dest;
    PX_PIXMAP SourcePixmap = FindPixmap(SourceId);

    ResolveDrawable(SourceId, &Source);
    ResolveDrawable(DestId, &Dest);
    if (!Source.Valid || !Dest.Valid)
    {
        if (Source.Clip)
            DeleteObject(Source.Clip);
        if (Dest.Clip)
            DeleteObject(Dest.Clip);
        return;
    }

    SelectClipRgn(Dest.Dc, Dest.Clip);
    if (SourcePixmap && SourcePixmap->Depth == 1)
    {
        BlitMono(&Dest,
                 Gc,
                 Source.Dc,
                 Source.OffsetX + SourceX,
                 Source.OffsetY + SourceY,
                 Width,
                 Height,
                 DestX,
                 DestY,
                 GxToSrcRop3(Gc ? Gc->Function : GXcopy));
    }
    else
    {
        HDC MonoDc = CreateCompatibleDC(Dest.Dc);
        HBITMAP MonoBitmap = CreateBitmap(Width, Height, 1, 1, NULL);

        if (MonoDc && MonoBitmap)
        {
            SelectObject(MonoDc, MonoBitmap);
            BitBlt(MonoDc,
                   0,
                   0,
                   Width,
                   Height,
                   Source.Dc,
                   Source.OffsetX + SourceX,
                   Source.OffsetY + SourceY,
                   SRCCOPY);
            BlitMono(&Dest,
                     Gc,
                     MonoDc,
                     0,
                     0,
                     Width,
                     Height,
                     DestX,
                     DestY,
                     GxToSrcRop3(Gc ? Gc->Function : GXcopy));
        }

        if (MonoDc)
            DeleteDC(MonoDc);
        if (MonoBitmap)
            DeleteObject(MonoBitmap);
    }
    SelectClipRgn(Dest.Dc, NULL);
    DeleteObject(Source.Clip);
    DeleteObject(Dest.Clip);
    InterlockedExchange(&g_Dirty, 1);
}

static
VOID
DoClearArea(
    _In_ PX_WINDOW Window,
    _In_ SHORT X,
    _In_ SHORT Y,
    _In_ WORD Width,
    _In_ WORD Height)
{
    X_TARGET Target;
    HRGN Region;
    RECT Rect;

    if (Width == 0)
        Width = (WORD)Window->Width;
    if (Height == 0)
        Height = (WORD)Window->Height;

    /* Clip to the window's own visible region so child windows are not painted over */
    ResolveDrawable(Window->Id, &Target);
    if (!Target.Valid)
    {
        if (Target.Clip)
            DeleteObject(Target.Clip);
        return;
    }

    Rect.left = Target.OffsetX + X;
    Rect.top = Target.OffsetY + Y;
    Rect.right = Rect.left + Width;
    Rect.bottom = Rect.top + Height;
    Region = BeginTarget(&Target);
    FillBackgroundRect(Window, &Rect);
    EndTarget(&Target, Region);
}

static
VOID
DoCopyArea(
    _In_ XID SourceId,
    _In_ XID DestId,
    _In_opt_ PX_GCONTEXT Gc,
    _In_ INT SourceX,
    _In_ INT SourceY,
    _In_ INT Width,
    _In_ INT Height,
    _In_ INT DestX,
    _In_ INT DestY)
{
    X_TARGET Source;
    X_TARGET Dest;
    PX_PIXMAP SourcePixmap = FindPixmap(SourceId);

    ResolveDrawable(SourceId, &Source);
    ResolveDrawable(DestId, &Dest);
    if (!Source.Valid || !Dest.Valid)
    {
        if (Source.Clip)
            DeleteObject(Source.Clip);
        if (Dest.Clip)
            DeleteObject(Dest.Clip);
        return;
    }

    SelectClipRgn(Dest.Dc, Dest.Clip);
    if (SourcePixmap && SourcePixmap->Depth == 1)
    {
        /* A depth mismatch is an X error, but clients doing it want the bitmap drawn */
        BlitMono(&Dest,
                 Gc,
                 Source.Dc,
                 Source.OffsetX + SourceX,
                 Source.OffsetY + SourceY,
                 Width,
                 Height,
                 DestX,
                 DestY,
                 GxToSrcRop3(Gc ? Gc->Function : GXcopy));
    }
    else
    {
        BitBlt(Dest.Dc,
               Dest.OffsetX + DestX,
               Dest.OffsetY + DestY,
               Width,
               Height,
               Source.Dc,
               Source.OffsetX + SourceX,
               Source.OffsetY + SourceY,
               GxToSrcRop3(Gc ? Gc->Function : GXcopy));
    }
    SelectClipRgn(Dest.Dc, NULL);
    DeleteObject(Source.Clip);
    DeleteObject(Dest.Clip);
    InterlockedExchange(&g_Dirty, 1);
}

/* Input routing */

/**
 * @brief Finds the deepest mapped window containing a screen point.
 * @return The window, or NULL if the point is outside Window.
 */
static
PX_WINDOW
WindowAtPoint(
    _In_ PX_WINDOW Window,
    _In_ INT PointX,
    _In_ INT PointY)
{
    PX_WINDOW Child;
    PX_WINDOW Hit = NULL;
    INT AbsX;
    INT AbsY;

    WindowAbsolutePosition(Window, &AbsX, &AbsY);
    if (PointX < AbsX || PointY < AbsY || PointX >= AbsX + Window->Width || PointY >= AbsY + Window->Height)
        return NULL;

    /* Topmost child first */
    for (Child = Window->LastChild; Child; Child = Child->PrevSibling)
    {
        if (!Child->Mapped)
            continue;

        Hit = WindowAtPoint(Child, PointX, PointY);
        if (Hit)
            return Hit;
    }
    return Window;
}

/**
 * @brief Sends EnterNotify or LeaveNotify to the clients selecting on Window.
 */
static
VOID
SendCrossing(
    _In_ PX_WINDOW Window,
    _In_ BYTE Code,
    _In_ DWORD Mask,
    _In_ INT PointX,
    _In_ INT PointY)
{
    BYTE Event[32];
    INT AbsX;
    INT AbsY;

    WindowAbsolutePosition(Window, &AbsX, &AbsY);
    RtlZeroMemory(Event, sizeof(Event));
    Event[0] = Code;
    *(DWORD *)(Event + 8) = ID_ROOT;
    *(DWORD *)(Event + 12) = (DWORD)Window->Id;
    *(WORD *)(Event + 20) = (WORD)PointX;
    *(WORD *)(Event + 22) = (WORD)PointY;
    *(WORD *)(Event + 24) = (WORD)(PointX - AbsX);
    *(WORD *)(Event + 26) = (WORD)(PointY - AbsY);
    Event[31] = 1;  /* same-screen */
    DeliverEvent(Window, Mask, Event);
}

/**
 * @brief Tracks the window under the pointer and emits Leave/Enter when it changes.
 */
static
VOID
UpdateCrossing(
    _In_ INT PointX,
    _In_ INT PointY)
{
    PX_WINDOW Current = WindowAtPoint(g_Root, PointX, PointY);
    PX_WINDOW Previous;

    if (!Current)
        Current = g_Root;
    if (Current->Id == g_PointerWindowId)
        return;

    Previous = FindWindowById(g_PointerWindowId);
    if (Previous)
        SendCrossing(Previous, LeaveNotify, LeaveWindowMask, PointX, PointY);
    SendCrossing(Current, EnterNotify, EnterWindowMask, PointX, PointY);
    g_PointerWindowId = Current->Id;
}

/**
 * @brief Delivers a device event to the window under the pointer, propagating up to the
 *        first ancestor that selected Mask.
 */
static
VOID
DeliverInput(
    _In_ BYTE Code,
    _In_ BYTE Detail,
    _In_ DWORD Mask,
    _In_ INT PointX,
    _In_ INT PointY)
{
    PX_WINDOW Window = WindowAtPoint(g_Root, PointX, PointY);
    PX_WINDOW EventWindow;

    if (!Window)
        Window = g_Root;

    for (EventWindow = Window; EventWindow; EventWindow = EventWindow->Parent)
    {
        INT Index;

        for (Index = 0; Index < EventWindow->SelectorCount; Index++)
        {
            if (EventWindow->Selectors[Index].Mask & Mask)
            {
                PX_CLIENT Client = EventWindow->Selectors[Index].Client;
                INT AbsX;
                INT AbsY;
                BYTE Event[32];

                WindowAbsolutePosition(EventWindow, &AbsX, &AbsY);
                RtlZeroMemory(Event, sizeof(Event));
                Event[0] = Code;
                Event[1] = Detail;
                Event[2] = (BYTE)(Client->Sequence & 0xFF);
                Event[3] = (BYTE)((Client->Sequence >> 8) & 0xFF);
                *(DWORD *)(Event + 8) = ID_ROOT;
                *(DWORD *)(Event + 12) = (DWORD)EventWindow->Id;
                *(WORD *)(Event + 20) = (WORD)PointX;
                *(WORD *)(Event + 22) = (WORD)PointY;
                *(WORD *)(Event + 24) = (WORD)(PointX - AbsX);
                *(WORD *)(Event + 26) = (WORD)(PointY - AbsY);
                Event[30] = 1;  /* same-screen */
                SendToClient(Client, Event, 32);
                return;
            }
        }
    }
}

/**
 * @brief Delivers a device event directly to a client with the given event window.
 */
static
VOID
DeliverToClient(
    _In_ PX_CLIENT Client,
    _In_ XID EventWindowId,
    _In_ BYTE Code,
    _In_ BYTE Detail,
    _In_ INT PointX,
    _In_ INT PointY)
{
    PX_WINDOW Window = FindWindowById(EventWindowId);
    INT AbsX = 0;
    INT AbsY = 0;
    BYTE Event[32];

    if (Window)
        WindowAbsolutePosition(Window, &AbsX, &AbsY);

    RtlZeroMemory(Event, sizeof(Event));
    Event[0] = Code;
    Event[1] = Detail;
    Event[2] = (BYTE)(Client->Sequence & 0xFF);
    Event[3] = (BYTE)((Client->Sequence >> 8) & 0xFF);
    *(DWORD *)(Event + 8) = ID_ROOT;
    *(DWORD *)(Event + 12) = EventWindowId;
    *(WORD *)(Event + 20) = (WORD)PointX;
    *(WORD *)(Event + 22) = (WORD)PointY;
    *(WORD *)(Event + 24) = (WORD)(PointX - AbsX);
    *(WORD *)(Event + 26) = (WORD)(PointY - AbsY);
    Event[30] = 1;
    SendToClient(Client, Event, 32);
}

/**
 * @brief Routes a pointer event, honoring active grabs and activating passive button grabs.
 */
static
VOID
RoutePointer(
    _In_ BYTE Code,
    _In_ BYTE Detail,
    _In_ DWORD Mask,
    _In_ INT PointX,
    _In_ INT PointY,
    _In_ BOOL IsPress,
    _In_ BOOL IsRelease)
{
    if (IsPress && !g_PointerGrab)
    {
        PX_WINDOW Window;

        /* The deepest window with a matching passive grab wins */
        for (Window = WindowAtPoint(g_Root, PointX, PointY);
             Window && !g_PointerGrab;
             Window = Window->Parent)
        {
            INT Index;

            for (Index = 0; Index < g_ButtonGrabCount; Index++)
            {
                if (g_ButtonGrabs[Index].Window == Window->Id &&
                    (g_ButtonGrabs[Index].Button == 0 || g_ButtonGrabs[Index].Button == Detail))
                {
                    g_PointerGrab = g_ButtonGrabs[Index].Client;
                    g_PointerGrabWindow = Window->Id;
                    g_PointerGrabExplicit = FALSE;
                    break;
                }
            }
        }
    }

    if (g_PointerGrab)
        DeliverToClient(g_PointerGrab, g_PointerGrabWindow, Code, Detail, PointX, PointY);
    else
        DeliverInput(Code, Detail, Mask, PointX, PointY);

    if (IsRelease && g_PointerGrab && !g_PointerGrabExplicit)
        g_PointerGrab = NULL;
}

/* Connection setup */

static
BOOL
SendSetupReply(
    _In_ PX_CLIENT Client)
{
    static const CHAR Vendor[] = "ReactOS-PSX";
    BYTE Buffer[512];
    PBYTE Out = Buffer;
    DWORD VendorLength = (DWORD)(sizeof(Vendor) - 1);
    DWORD VendorPad = (4 - (VendorLength & 3)) & 3;
    DWORD AdditionalBytes;

#define PUT8(v)  (*Out++ = (BYTE)(v))
#define PUT16(v) do { WORD _w = (WORD)(v); *Out++ = _w & 0xFF; *Out++ = (_w >> 8) & 0xFF; } while (0)
#define PUT32(v) do { DWORD _d = (DWORD)(v); *Out++ = _d & 0xFF; *Out++ = (_d >> 8) & 0xFF; \
                      *Out++ = (_d >> 16) & 0xFF; *Out++ = (_d >> 24) & 0xFF; } while (0)

    /* Prefix: success, protocol 11.0, additional length patched below */
    PUT8(1);
    PUT8(0);
    PUT16(11);
    PUT16(0);
    PUT16(0);

    PUT32(1);                   /* release */
    PUT32(Client->IdBase);      /* resource id base */
    PUT32(Client->IdMask);      /* resource id mask */
    PUT32(0);                   /* motion buffer size */
    PUT16(VendorLength);
    PUT16(65535);               /* maximum request length */
    PUT8(1);                    /* screens */
    PUT8(1);                    /* pixmap formats */
    PUT8(0);                    /* image byte order */
    PUT8(0);                    /* bitmap bit order */
    PUT8(32);                   /* scanline unit */
    PUT8(32);                   /* scanline pad */
    PUT8(8);                    /* min keycode */
    PUT8(255);                  /* max keycode */
    PUT32(0);
    RtlCopyMemory(Out, Vendor, VendorLength);
    Out += VendorLength;
    while (VendorPad--)
        PUT8(0);

    /* Pixmap format */
    PUT8(24);
    PUT8(32);
    PUT8(32);
    PUT8(0);
    PUT32(0);

    /* Screen */
    PUT32(ID_ROOT);
    PUT32(ID_COLORMAP);
    PUT32(WHITE_PIXEL);
    PUT32(BLACK_PIXEL);
    PUT32(0);                   /* current input masks */
    PUT16(SCREEN_W);
    PUT16(SCREEN_H);
    PUT16(211);                 /* width in mm */
    PUT16(158);                 /* height in mm */
    PUT16(1);                   /* min installed maps */
    PUT16(1);                   /* max installed maps */
    PUT32(ID_VISUAL);
    PUT8(0);                    /* backing stores */
    PUT8(0);                    /* save unders */
    PUT8(24);                   /* root depth */
    PUT8(1);                    /* allowed depths */

    /* Depth */
    PUT8(24);
    PUT8(0);
    PUT16(1);
    PUT32(0);

    /* Visual type: TrueColor, 8 bits per RGB, 256 colormap entries */
    PUT32(ID_VISUAL);
    PUT8(4);
    PUT8(8);
    PUT16(256);
    PUT32(0x00FF0000);
    PUT32(0x0000FF00);
    PUT32(0x000000FF);
    PUT32(0);

    AdditionalBytes = (DWORD)(Out - Buffer) - 8;
    Buffer[6] = (BYTE)((AdditionalBytes / 4) & 0xFF);
    Buffer[7] = (BYTE)(((AdditionalBytes / 4) >> 8) & 0xFF);
#undef PUT8
#undef PUT16
#undef PUT32

    SendToClient(Client, Buffer, (DWORD)(Out - Buffer));
    return TRUE;
}

/* Request helpers */

/**
 * @brief Sets a client's event mask on a window, replacing any previous mask from that client.
 */
static
VOID
AddSelector(
    _In_ PX_WINDOW Window,
    _In_ PX_CLIENT Client,
    _In_ DWORD Mask)
{
    INT Index;

    if (Window == g_Root)
        XWmLog("c%lu selroot %lx", Client->Id, Mask);

    for (Index = 0; Index < Window->SelectorCount; Index++)
    {
        if (Window->Selectors[Index].Client == Client)
        {
            Window->Selectors[Index].Mask = Mask;
            return;
        }
    }

    if (Window->SelectorCount < MAX_SELECT)
    {
        Window->Selectors[Window->SelectorCount].Client = Client;
        Window->Selectors[Window->SelectorCount].Mask = Mask;
        Window->SelectorCount++;
    }
}

/* Window property store */

static
PX_PROPERTY
FindProperty(
    _In_ PX_WINDOW Window,
    _In_ XID Atom)
{
    PX_PROPERTY Property;

    for (Property = Window->Properties; Property; Property = Property->Next)
    {
        if (Property->Atom == Atom)
            return Property;
    }
    return NULL;
}

#define PROP_MAX_BYTES 0x40000

/**
 * @brief Stores property data. Mode is 0 Replace, 1 Prepend or 2 Append.
 */
static
BOOL
StoreProperty(
    _In_ PX_WINDOW Window,
    _In_ XID Atom,
    _In_ XID Type,
    _In_ BYTE Format,
    _In_ BYTE Mode,
    _In_reads_bytes_(Length) const BYTE *Data,
    _In_ DWORD Length)
{
    PX_PROPERTY Property = FindProperty(Window, Atom);

    if (Format != 8 && Format != 16 && Format != 32)
        return FALSE;

    /* Prepend and append must match the existing type and format */
    if (Mode != 0 && Property && (Property->Type != Type || Property->Format != Format))
        return FALSE;

    if (!Property)
    {
        Property = (PX_PROPERTY)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Property));
        if (!Property)
            return FALSE;

        Property->Atom = Atom;
        Property->Next = Window->Properties;
        Window->Properties = Property;
        Mode = 0;
    }

    if (Mode == 0)
    {
        PBYTE NewData = NULL;

        if (Length > PROP_MAX_BYTES)
            Length = PROP_MAX_BYTES;

        if (Length)
        {
            NewData = (PBYTE)HeapAlloc(GetProcessHeap(), 0, Length);
            if (!NewData)
                return FALSE;
            RtlCopyMemory(NewData, Data, Length);
        }

        if (Property->Data)
            HeapFree(GetProcessHeap(), 0, Property->Data);
        Property->Data = NewData;
        Property->Length = Length;
        Property->Type = Type;
        Property->Format = Format;
    }
    else
    {
        DWORD Total = Property->Length + Length;
        PBYTE NewData;

        if (Total > PROP_MAX_BYTES || Total < Property->Length)
            return FALSE;

        NewData = (PBYTE)HeapAlloc(GetProcessHeap(), 0, Total ? Total : 1);
        if (!NewData)
            return FALSE;

        if (Mode == 1)
        {
            RtlCopyMemory(NewData, Data, Length);
            RtlCopyMemory(NewData + Length, Property->Data, Property->Length);
        }
        else
        {
            RtlCopyMemory(NewData, Property->Data, Property->Length);
            RtlCopyMemory(NewData + Property->Length, Data, Length);
        }

        if (Property->Data)
            HeapFree(GetProcessHeap(), 0, Property->Data);
        Property->Data = NewData;
        Property->Length = Total;
    }
    return TRUE;
}

static
VOID
DeleteXProperty(
    _In_ PX_WINDOW Window,
    _In_ XID Atom)
{
    PX_PROPERTY *Link;

    for (Link = &Window->Properties; *Link; Link = &(*Link)->Next)
    {
        if ((*Link)->Atom == Atom)
        {
            PX_PROPERTY Property = *Link;

            *Link = Property->Next;
            if (Property->Data)
                HeapFree(GetProcessHeap(), 0, Property->Data);
            HeapFree(GetProcessHeap(), 0, Property);
            return;
        }
    }
}

static
VOID
FreeAllProperties(
    _In_ PX_WINDOW Window)
{
    while (Window->Properties)
        DeleteXProperty(Window, Window->Properties->Atom);
}

/**
 * @brief Sends PropertyNotify. State is 0 NewValue or 1 Deleted.
 */
static
VOID
SendPropertyNotify(
    _In_ PX_WINDOW Window,
    _In_ XID Atom,
    _In_ BYTE State)
{
    BYTE Event[32];

    RtlZeroMemory(Event, sizeof(Event));
    Event[0] = PropertyNotify;
    *(DWORD *)(Event + 4) = (DWORD)Window->Id;
    *(DWORD *)(Event + 8) = (DWORD)Atom;
    *(DWORD *)(Event + 12) = g_ServerTime++;    /* time, must be nonzero */
    Event[16] = State;
    DeliverEvent(Window, PropertyChangeMask, Event);
}

/**
 * @brief Extracts the value for Bit from a mask-indexed value list.
 */
static
DWORD
GetMaskedValue(
    _In_ DWORD Mask,
    _In_ DWORD Bit,
    _In_reads_(Count) const DWORD *Values,
    _In_ DWORD Count)
{
    DWORD Index = 0;
    DWORD Walk;

    if (!(Mask & Bit))
        return 0;

    for (Walk = 1; Walk < Bit; Walk <<= 1)
    {
        if (Mask & Walk)
            Index++;
    }
    return (Index < Count) ? Values[Index] : 0;
}

/**
 * @brief Applies a CreateGC/ChangeGC value list to a GC.
 */
static
VOID
ParseGcValues(
    _Inout_ PX_GCONTEXT Gc,
    _In_ DWORD Mask,
    _In_reads_(Count) const DWORD *Values,
    _In_ DWORD Count)
{
    if (Mask & GCFunction)
        Gc->Function = GetMaskedValue(Mask, GCFunction, Values, Count);
    if (Mask & GCPlaneMask)
        Gc->PlaneMask = GetMaskedValue(Mask, GCPlaneMask, Values, Count);
    if (Mask & GCForeground)
        Gc->Foreground = GetMaskedValue(Mask, GCForeground, Values, Count);
    if (Mask & GCBackground)
        Gc->Background = GetMaskedValue(Mask, GCBackground, Values, Count);
    if (Mask & GCLineWidth)
        Gc->LineWidth = GetMaskedValue(Mask, GCLineWidth, Values, Count);
    if (Mask & GCLineStyle)
        Gc->LineStyle = GetMaskedValue(Mask, GCLineStyle, Values, Count);
    if (Mask & GCCapStyle)
        Gc->CapStyle = GetMaskedValue(Mask, GCCapStyle, Values, Count);
    if (Mask & GCJoinStyle)
        Gc->JoinStyle = GetMaskedValue(Mask, GCJoinStyle, Values, Count);
    if (Mask & GCFillStyle)
        Gc->FillStyle = GetMaskedValue(Mask, GCFillStyle, Values, Count);
    if (Mask & GCFillRule)
        Gc->FillRule = GetMaskedValue(Mask, GCFillRule, Values, Count);
    if (Mask & GCTile)
        Gc->Tile = GetMaskedValue(Mask, GCTile, Values, Count);
    if (Mask & GCStipple)
        Gc->Stipple = GetMaskedValue(Mask, GCStipple, Values, Count);
    if (Mask & GCTileStipXOrigin)
        Gc->TileStipXOrigin = (SHORT)GetMaskedValue(Mask, GCTileStipXOrigin, Values, Count);
    if (Mask & GCTileStipYOrigin)
        Gc->TileStipYOrigin = (SHORT)GetMaskedValue(Mask, GCTileStipYOrigin, Values, Count);
    if (Mask & GCFont)
        Gc->Font = GetMaskedValue(Mask, GCFont, Values, Count);
    if (Mask & GCSubwindowMode)
        Gc->SubwindowMode = GetMaskedValue(Mask, GCSubwindowMode, Values, Count);
    if (Mask & GCGraphicsExposures)
        Gc->GraphicsExposures = GetMaskedValue(Mask, GCGraphicsExposures, Values, Count);
    if (Mask & GCClipXOrigin)
        Gc->ClipXOrigin = (SHORT)GetMaskedValue(Mask, GCClipXOrigin, Values, Count);
    if (Mask & GCClipYOrigin)
        Gc->ClipYOrigin = (SHORT)GetMaskedValue(Mask, GCClipYOrigin, Values, Count);

    if (Mask & GCClipMask)
    {
        Gc->ClipMask = GetMaskedValue(Mask, GCClipMask, Values, Count);

        /* Setting a clip mask, even None, replaces any SetClipRectangles list */
        if (Gc->ClipRects)
        {
            HeapFree(GetProcessHeap(), 0, Gc->ClipRects);
            Gc->ClipRects = NULL;
        }
        Gc->ClipRectCount = 0;
    }

    if (Mask & GCDashOffset)
        Gc->DashOffset = GetMaskedValue(Mask, GCDashOffset, Values, Count);

    if (Mask & GCDashList)
    {
        Gc->Dashes[0] = (BYTE)GetMaskedValue(Mask, GCDashList, Values, Count);
        Gc->DashCount = 1;
    }

    if (Mask & GCArcMode)
        Gc->ArcMode = GetMaskedValue(Mask, GCArcMode, Values, Count);
}

static
VOID
DoMapWindow(
    _In_ PX_CLIENT Client,
    _In_ PX_WINDOW Window)
{
    PX_CLIENT WindowManager;

    if (Window->Mapped)
        return;

    WindowManager = Window->Parent ? FindRedirectClient(Window->Parent, Client) : NULL;
    if (WindowManager && !Window->OverrideRedirect)
    {
        BYTE Event[32];

        /* Redirected: send MapRequest to the window manager instead of mapping */
        XWmLog("mapreq %lx>c%lu", Window->Id, WindowManager->Id);
        RtlZeroMemory(Event, sizeof(Event));
        Event[0] = MapRequest;
        Event[2] = (BYTE)(WindowManager->Sequence & 0xFF);
        Event[3] = (BYTE)((WindowManager->Sequence >> 8) & 0xFF);
        *(DWORD *)(Event + 4) = (DWORD)Window->Parent->Id;
        *(DWORD *)(Event + 8) = (DWORD)Window->Id;
        SendToClient(WindowManager, Event, 32);
        return;
    }

    if (Window->Parent == g_Root && !Window->OverrideRedirect && Window != g_Root)
        XWmLog("map %lx DIR", Window->Id);

    Window->Mapped = TRUE;
    SendMapNotify(Window);
    RepaintScreen();
}

static
VOID
DoConfigureWindow(
    _In_ PX_CLIENT Client,
    _In_ PX_WINDOW Window,
    _In_ DWORD Mask,
    _In_reads_(Count) const DWORD *Values,
    _In_ DWORD Count)
{
    PX_CLIENT WindowManager = Window->Parent ? FindRedirectClient(Window->Parent, Client) : NULL;

    if (WindowManager && !Window->OverrideRedirect)
    {
        BYTE Event[32];

        RtlZeroMemory(Event, sizeof(Event));
        Event[0] = ConfigureRequest;
        Event[2] = (BYTE)(WindowManager->Sequence & 0xFF);
        Event[3] = (BYTE)((WindowManager->Sequence >> 8) & 0xFF);
        *(DWORD *)(Event + 4) = (DWORD)Window->Parent->Id;
        *(DWORD *)(Event + 8) = (DWORD)Window->Id;
        if (Mask & CWConfX)
            *(WORD *)(Event + 16) = (WORD)GetMaskedValue(Mask, CWConfX, Values, Count);
        if (Mask & CWConfY)
            *(WORD *)(Event + 18) = (WORD)GetMaskedValue(Mask, CWConfY, Values, Count);
        if (Mask & CWConfW)
            *(WORD *)(Event + 20) = (WORD)GetMaskedValue(Mask, CWConfW, Values, Count);
        if (Mask & CWConfH)
            *(WORD *)(Event + 22) = (WORD)GetMaskedValue(Mask, CWConfH, Values, Count);
        *(WORD *)(Event + 26) = (WORD)Mask;
        SendToClient(WindowManager, Event, 32);
        return;
    }

    if (Mask & CWConfX)
        Window->X = (SHORT)GetMaskedValue(Mask, CWConfX, Values, Count);
    if (Mask & CWConfY)
        Window->Y = (SHORT)GetMaskedValue(Mask, CWConfY, Values, Count);
    if (Mask & CWConfW)
        Window->Width = (WORD)GetMaskedValue(Mask, CWConfW, Values, Count);
    if (Mask & CWConfH)
        Window->Height = (WORD)GetMaskedValue(Mask, CWConfH, Values, Count);
    if (Mask & CWConfBorder)
        Window->BorderWidth = (WORD)GetMaskedValue(Mask, CWConfBorder, Values, Count);

    if (Mask & CWConfStack)
    {
        DWORD StackMode = GetMaskedValue(Mask, CWConfStack, Values, Count);

        if (StackMode == 0)
        {
            /* Above */
            RaiseXWindow(Window);
        }
        else if (StackMode == 1 && Window->Parent)
        {
            /* Below: move to the bottom of the sibling list */
            PX_WINDOW Parent = Window->Parent;

            TreeUnlink(Window);
            Window->Parent = Parent;
            Window->PrevSibling = NULL;
            Window->NextSibling = Parent->FirstChild;
            if (Parent->FirstChild)
                Parent->FirstChild->PrevSibling = Window;
            else
                Parent->LastChild = Window;
            Parent->FirstChild = Window;
        }
    }

    SendConfigureNotify(Window);
    RepaintScreen();
}

/**
 * @brief Returns TRUE for core requests that have a reply. Unimplemented ones must be
 *        answered with an error or the client waits forever.
 */
static
BOOL
IsReplyOpcode(
    _In_ BYTE Opcode)
{
    switch (Opcode)
    {
        case 3:   /* GetWindowAttributes */
        case 14:  /* GetGeometry */
        case 15:  /* QueryTree */
        case 16:  /* InternAtom */
        case 17:  /* GetAtomName */
        case 20:  /* GetProperty */
        case 21:  /* ListProperties */
        case 23:  /* GetSelectionOwner */
        case 26:  /* GrabPointer */
        case 31:  /* GrabKeyboard */
        case 38:  /* QueryPointer */
        case 39:  /* GetMotionEvents */
        case 40:  /* TranslateCoords */
        case 43:  /* GetInputFocus */
        case 44:  /* QueryKeymap */
        case 47:  /* QueryFont */
        case 48:  /* QueryTextExtents */
        case 49:  /* ListFonts */
        case 50:  /* ListFontsWithInfo */
        case 52:  /* GetFontPath */
        case 73:  /* GetImage */
        case 83:  /* ListInstalledCmaps */
        case 84:  /* AllocColor */
        case 85:  /* AllocNamedColor */
        case 86:  /* AllocColorCells */
        case 87:  /* AllocColorPlanes */
        case 91:  /* QueryColors */
        case 92:  /* LookupColor */
        case 97:  /* QueryBestSize */
        case 98:  /* QueryExtension */
        case 99:  /* ListExtensions */
        case 101: /* GetKeyboardMapping */
        case 103: /* GetKeyboardControl */
        case 106: /* GetPointerControl */
        case 108: /* GetScreenSaver */
        case 110: /* ListHosts */
        case 116: /* SetPointerMapping */
        case 117: /* GetPointerMapping */
        case 118: /* SetModifierMapping */
        case 119: /* GetModifierMapping */
            return TRUE;

        default:
            return FALSE;
    }
}

/**
 * @brief Maps a keycode to a US layout keysym. Keycodes are Windows virtual key codes.
 * @return The keysym, or NoSymbol (0).
 */
static
DWORD
KeycodeToKeysym(
    _In_ BYTE VirtualKey,
    _In_ INT Shift)
{
    /* Letters */
    if (VirtualKey >= 'A' && VirtualKey <= 'Z')
        return Shift ? (DWORD)VirtualKey : (DWORD)(VirtualKey + 32);

    /* Digits and their shifted symbols */
    if (VirtualKey >= '0' && VirtualKey <= '9')
    {
        static const CHAR Shifted[] = ")!@#$%^&*(";

        return Shift ? (DWORD)Shifted[VirtualKey - '0'] : (DWORD)VirtualKey;
    }

    /* F1 through F12 */
    if (VirtualKey >= 0x70 && VirtualKey <= 0x7B)
        return 0xFFBE + (VirtualKey - 0x70);

    switch (VirtualKey)
    {
        case 0x20: return 0x20;                     /* space */
        case 0x0D: return 0xFF0D;                   /* Return */
        case 0x08: return 0xFF08;                   /* BackSpace */
        case 0x09: return 0xFF09;                   /* Tab */
        case 0x1B: return 0xFF1B;                   /* Escape */
        case 0x2E: return 0xFFFF;                   /* Delete */
        case 0x2D: return 0xFF63;                   /* Insert */
        case 0x24: return 0xFF50;                   /* Home */
        case 0x23: return 0xFF57;                   /* End */
        case 0x21: return 0xFF55;                   /* Page_Up */
        case 0x22: return 0xFF56;                   /* Page_Down */
        case 0x25: return 0xFF51;                   /* Left */
        case 0x26: return 0xFF52;                   /* Up */
        case 0x27: return 0xFF53;                   /* Right */
        case 0x28: return 0xFF54;                   /* Down */
        case 0x10: return 0xFFE1;                   /* Shift_L */
        case 0x11: return 0xFFE3;                   /* Control_L */
        case 0x12: return 0xFFE9;                   /* Alt_L */
        case 0x14: return 0xFFE5;                   /* Caps_Lock */
        case 0xBA: return Shift ? ':' : ';';        /* OEM_1 */
        case 0xBB: return Shift ? '+' : '=';        /* OEM_PLUS */
        case 0xBC: return Shift ? '<' : ',';        /* OEM_COMMA */
        case 0xBD: return Shift ? '_' : '-';        /* OEM_MINUS */
        case 0xBE: return Shift ? '>' : '.';        /* OEM_PERIOD */
        case 0xBF: return Shift ? '?' : '/';        /* OEM_2 */
        case 0xC0: return Shift ? '~' : '`';        /* OEM_3 */
        case 0xDB: return Shift ? '{' : '[';        /* OEM_4 */
        case 0xDC: return Shift ? '|' : '\\';       /* OEM_5 */
        case 0xDD: return Shift ? '}' : ']';        /* OEM_6 */
        case 0xDE: return Shift ? '"' : '\'';       /* OEM_7 */
    }
    return 0;
}

/* Request handlers */

/**
 * @brief Applies a CreateWindow/ChangeWindowAttributes value list.
 */
static
VOID
ApplyWindowAttributes(
    _In_ PX_CLIENT Client,
    _Inout_ PX_WINDOW Window,
    _In_ DWORD Mask,
    _In_reads_(Count) const DWORD *Values,
    _In_ DWORD Count)
{
    if (Mask & CWBackPixmap)
    {
        Window->BackgroundPixmap = GetMaskedValue(Mask, CWBackPixmap, Values, Count);
        Window->HasBackground = FALSE;
    }
    if (Mask & CWBackPixel)
    {
        Window->BackgroundPixel = GetMaskedValue(Mask, CWBackPixel, Values, Count);
        Window->HasBackground = TRUE;
        Window->BackgroundPixmap = 0;
    }
    if (Mask & CWBorderPixel)
        Window->BorderPixel = GetMaskedValue(Mask, CWBorderPixel, Values, Count);
    if (Mask & CWOverrideRedirect)
        Window->OverrideRedirect = GetMaskedValue(Mask, CWOverrideRedirect, Values, Count) != 0;
    if (Mask & CWEventMask)
        AddSelector(Window, Client, GetMaskedValue(Mask, CWEventMask, Values, Count));
}

static
VOID
HandleCreateWindow(
    _In_ PX_CLIENT Client,
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength)
{
    XID WindowId;
    PX_WINDOW Parent;
    PX_WINDOW Window;

    /* wid@0 parent@4 x@8 y@10 width@12 height@14 border@16 class@18 visual@20 mask@24 values@28 */
    if (BodyLength < 28)
        return;

    WindowId = *(const DWORD *)(Body + 0);
    Parent = FindWindowById(*(const DWORD *)(Body + 4));
    if (!Parent)
        Parent = g_Root;

    Window = AllocateWindow(WindowId,
                            Parent,
                            *(const SHORT *)(Body + 8),
                            *(const SHORT *)(Body + 10),
                            *(const WORD *)(Body + 12),
                            *(const WORD *)(Body + 14));
    if (!Window)
        return;

    Window->Owner = Client;
    Window->BorderWidth = *(const WORD *)(Body + 16);
    ApplyWindowAttributes(Client,
                          Window,
                          *(const DWORD *)(Body + 24),
                          (const DWORD *)(Body + 28),
                          (BodyLength - 28) / 4);
    SendCreateNotify(Window);
}

static
VOID
HandleGetWindowAttributes(
    _In_ PX_CLIENT Client,
    _In_reads_bytes_(4) const BYTE *Body)
{
    PX_WINDOW Window = FindWindowById(*(const DWORD *)Body);
    BYTE Reply[44];

    RtlZeroMemory(Reply, sizeof(Reply));
    InitReplyHeader(Reply, Client, 3);
    Reply[1] = 0;
    *(DWORD *)(Reply + 8) = ID_VISUAL;
    *(WORD *)(Reply + 12) = 1;                          /* InputOutput */
    Reply[26] = (BYTE)(Window && Window->Mapped ? 2 : 0);   /* map-state */
    Reply[27] = (BYTE)(Window ? Window->OverrideRedirect : 0);
    *(DWORD *)(Reply + 28) = ID_COLORMAP;
    if (Window)
        *(DWORD *)(Reply + 36) = Window->SelectorCount ? Window->Selectors[0].Mask : 0;
    SendToClient(Client, Reply, 44);
}

static
VOID
HandleDestroyWindow(
    _In_reads_bytes_(4) const BYTE *Body)
{
    PX_WINDOW Window = FindWindowById(*(const DWORD *)Body);
    PX_WINDOW *Link;

    if (!Window || Window == g_Root)
        return;

    SendDestroyNotify(Window);
    Window->Mapped = FALSE;
    TreeUnlink(Window);
    for (Link = &g_AllWindows; *Link; Link = &(*Link)->AllNext)
    {
        if (*Link == Window)
        {
            *Link = Window->AllNext;
            break;
        }
    }
    FreeAllProperties(Window);
    HeapFree(GetProcessHeap(), 0, Window);
    RepaintScreen();
}

static
VOID
HandleReparentWindow(
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength)
{
    PX_WINDOW Window;
    PX_WINDOW NewParent;
    BOOL WasMapped;

    if (BodyLength < 12)
        return;

    Window = FindWindowById(*(const DWORD *)(Body + 0));
    NewParent = FindWindowById(*(const DWORD *)(Body + 4));
    if (!Window || !NewParent || Window == g_Root)
        return;

    WasMapped = Window->Mapped;
    XWmLog("repar %lx>%lx", Window->Id, NewParent->Id);
    if (WasMapped)
        Window->Mapped = FALSE;

    TreeUnlink(Window);
    TreeLink(NewParent, Window);
    Window->X = *(const SHORT *)(Body + 8);
    Window->Y = *(const SHORT *)(Body + 10);
    SendReparentNotify(Window);
    if (WasMapped)
    {
        Window->Mapped = TRUE;
        SendMapNotify(Window);
    }
    RepaintScreen();
}

static
VOID
HandleGetGeometry(
    _In_ PX_CLIENT Client,
    _In_reads_bytes_(4) const BYTE *Body)
{
    PX_WINDOW Window = FindWindowById(*(const DWORD *)Body);
    BYTE Reply[32];

    InitReplyHeader(Reply, Client, 0);
    Reply[1] = 24;                          /* depth */
    *(DWORD *)(Reply + 8) = ID_ROOT;
    if (Window)
    {
        *(WORD *)(Reply + 12) = (WORD)Window->X;
        *(WORD *)(Reply + 14) = (WORD)Window->Y;
        *(WORD *)(Reply + 16) = (WORD)Window->Width;
        *(WORD *)(Reply + 18) = (WORD)Window->Height;
        *(WORD *)(Reply + 20) = (WORD)Window->BorderWidth;
    }
    else
    {
        *(WORD *)(Reply + 16) = SCREEN_W;
        *(WORD *)(Reply + 18) = SCREEN_H;
    }
    SendToClient(Client, Reply, 32);
}

static
VOID
HandleQueryTree(
    _In_ PX_CLIENT Client,
    _In_reads_bytes_(4) const BYTE *Body)
{
    PX_WINDOW Window = FindWindowById(*(const DWORD *)Body);
    BYTE Reply[512];
    WORD ChildCount = 0;
    DWORD Offset = 32;

    RtlZeroMemory(Reply, sizeof(Reply));
    Reply[0] = 1;
    Reply[2] = (BYTE)(Client->Sequence & 0xFF);
    Reply[3] = (BYTE)((Client->Sequence >> 8) & 0xFF);
    *(DWORD *)(Reply + 8) = ID_ROOT;
    *(DWORD *)(Reply + 12) = (Window && Window->Parent) ? (DWORD)Window->Parent->Id : 0;
    if (Window)
    {
        PX_WINDOW Child;

        for (Child = Window->FirstChild;
             Child && Offset + 4 <= sizeof(Reply);
             Child = Child->NextSibling)
        {
            *(DWORD *)(Reply + Offset) = (DWORD)Child->Id;
            Offset += 4;
            ChildCount++;
        }
    }
    *(WORD *)(Reply + 16) = ChildCount;
    *(DWORD *)(Reply + 4) = ChildCount;
    SendToClient(Client, Reply, 32 + ChildCount * 4);
}

static
VOID
HandleInternAtom(
    _In_ PX_CLIENT Client,
    _In_ PCX_REQ_HEAD Header,
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength)
{
    BYTE Reply[32];
    XID Atom = 0;

    /* only-if-exists in the header data byte, name length@0, name@4 */
    if (BodyLength >= 4)
    {
        INT NameLength = *(const WORD *)(Body + 0);

        if ((DWORD)(4 + NameLength) <= BodyLength)
            Atom = InternAtomByName((PCSTR)(Body + 4), NameLength, Header->data != 0);
    }

    InitReplyHeader(Reply, Client, 0);
    *(DWORD *)(Reply + 8) = (DWORD)Atom;
    SendToClient(Client, Reply, 32);
}

static
VOID
HandleGetAtomName(
    _In_ PX_CLIENT Client,
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength)
{
    PCSTR Name = (BodyLength >= 4) ? GetAtomNameById(*(const DWORD *)(Body + 0)) : NULL;
    INT NameLength = 0;
    BYTE Reply[32 + ATOM_NAME_MAX + 4];

    if (Name)
    {
        while (Name[NameLength] != '\0')
            NameLength++;
    }

    RtlZeroMemory(Reply, sizeof(Reply));
    InitReplyHeader(Reply, Client, ((DWORD)NameLength + 3) / 4);
    *(WORD *)(Reply + 8) = (WORD)NameLength;
    if (Name)
        RtlCopyMemory(Reply + 32, Name, NameLength);
    SendToClient(Client, Reply, 32 + (((DWORD)NameLength + 3) & ~3u));
}

static
VOID
HandleChangeProperty(
    _In_ PCX_REQ_HEAD Header,
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength)
{
    PX_WINDOW Window;
    XID Atom;
    BYTE Format;
    DWORD Bytes;

    /* mode in the header data byte, window@0 property@4 type@8 format@12 length@16 data@20 */
    if (BodyLength < 20)
        return;

    Window = FindWindowById(*(const DWORD *)(Body + 0));
    Atom = *(const DWORD *)(Body + 4);
    if (!Window)
        return;

    Format = Body[12];
    Bytes = *(const DWORD *)(Body + 16) * (Format / 8);
    if (20 + Bytes <= BodyLength)
        StoreProperty(Window, Atom, *(const DWORD *)(Body + 8), Format, Header->data, Body + 20, Bytes);

    /* Always notify; zero-length appends are used to obtain a server timestamp */
    SendPropertyNotify(Window, Atom, 0);
}

static
VOID
HandleDeleteProperty(
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength)
{
    PX_WINDOW Window;
    XID Atom;

    if (BodyLength < 8)
        return;

    Window = FindWindowById(*(const DWORD *)(Body + 0));
    Atom = *(const DWORD *)(Body + 4);
    if (Window && FindProperty(Window, Atom))
    {
        DeleteXProperty(Window, Atom);
        SendPropertyNotify(Window, Atom, 1);
    }
}

static
VOID
HandleGetProperty(
    _In_ PX_CLIENT Client,
    _In_ PCX_REQ_HEAD Header,
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength)
{
    /* delete in the header data byte, window@0 property@4 type@8 long-offset@12 long-length@16 */
    PX_WINDOW Window = (BodyLength >= 20) ? FindWindowById(*(const DWORD *)(Body + 0)) : NULL;
    PX_PROPERTY Property = Window ? FindProperty(Window, *(const DWORD *)(Body + 4)) : NULL;
    XID RequestedType = (BodyLength >= 20) ? *(const DWORD *)(Body + 8) : 0;
    BYTE Reply[32];
    DWORD Offset;
    DWORD MaxOut;
    DWORD Available;
    DWORD OutLength;
    DWORD Padded;
    PBYTE Full;

    if (!Property)
    {
        /* Nonexistent: type None */
        InitReplyHeader(Reply, Client, 0);
        SendToClient(Client, Reply, 32);
        return;
    }

    if (RequestedType != 0 && RequestedType != Property->Type)
    {
        /* Type mismatch: report the actual type, format and length without data */
        InitReplyHeader(Reply, Client, 0);
        Reply[1] = Property->Format;
        *(DWORD *)(Reply + 8) = (DWORD)Property->Type;
        *(DWORD *)(Reply + 12) = Property->Length;
        SendToClient(Client, Reply, 32);
        return;
    }

    Offset = *(const DWORD *)(Body + 12) * 4;
    MaxOut = *(const DWORD *)(Body + 16) * 4;
    Available = (Offset < Property->Length) ? Property->Length - Offset : 0;
    OutLength = (Available < MaxOut) ? Available : MaxOut;
    Padded = (OutLength + 3) & ~3u;

    Full = (PBYTE)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 32 + Padded);
    if (!Full)
    {
        InitReplyHeader(Reply, Client, 0);
        SendToClient(Client, Reply, 32);
        return;
    }

    RtlZeroMemory(Full, 32);
    Full[0] = 1;
    Full[1] = Property->Format;
    Full[2] = (BYTE)(Client->Sequence & 0xFF);
    Full[3] = (BYTE)((Client->Sequence >> 8) & 0xFF);
    *(DWORD *)(Full + 4) = Padded / 4;
    *(DWORD *)(Full + 8) = (DWORD)Property->Type;
    *(DWORD *)(Full + 12) = Available - OutLength;              /* bytes-after */
    *(DWORD *)(Full + 16) = OutLength / (Property->Format / 8); /* length in format units */
    if (OutLength)
        RtlCopyMemory(Full + 32, Property->Data + Offset, OutLength);
    SendToClient(Client, Full, 32 + Padded);
    HeapFree(GetProcessHeap(), 0, Full);

    /* Delete once fully read */
    if (Header->data && Available - OutLength == 0)
    {
        XID Atom = Property->Atom;

        DeleteXProperty(Window, Atom);
        SendPropertyNotify(Window, Atom, 1);
    }
}

static
VOID
HandleListProperties(
    _In_ PX_CLIENT Client,
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength)
{
    PX_WINDOW Window = (BodyLength >= 4) ? FindWindowById(*(const DWORD *)(Body + 0)) : NULL;
    PX_PROPERTY Property;
    WORD Count = 0;
    BYTE Reply[32 + 4 * 64];

    RtlZeroMemory(Reply, sizeof(Reply));
    if (Window)
    {
        for (Property = Window->Properties; Property && Count < 64; Property = Property->Next, Count++)
            *(DWORD *)(Reply + 32 + 4 * Count) = (DWORD)Property->Atom;
    }
    Reply[0] = 1;
    Reply[2] = (BYTE)(Client->Sequence & 0xFF);
    Reply[3] = (BYTE)((Client->Sequence >> 8) & 0xFF);
    *(DWORD *)(Reply + 4) = Count;
    *(WORD *)(Reply + 8) = Count;
    SendToClient(Client, Reply, 32 + 4 * Count);
}

static
VOID
HandleUngrabButton(
    _In_ PX_CLIENT Client,
    _In_ PCX_REQ_HEAD Header,
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength)
{
    XID Window;
    INT Button;
    INT Index;
    INT Kept;

    /* button in the header data byte (0 is AnyButton), grab window@0; only matching grabs go */
    if (BodyLength < 4)
        return;

    Window = *(const DWORD *)(Body + 0);
    Button = Header->data;
    for (Index = 0, Kept = 0; Index < g_ButtonGrabCount; Index++)
    {
        if (g_ButtonGrabs[Index].Client == Client &&
            g_ButtonGrabs[Index].Window == Window &&
            (Button == 0 || g_ButtonGrabs[Index].Button == 0 || g_ButtonGrabs[Index].Button == Button))
        {
            continue;
        }
        g_ButtonGrabs[Kept++] = g_ButtonGrabs[Index];
    }
    g_ButtonGrabCount = Kept;
}

static
VOID
HandleQueryBestSize(
    _In_ PX_CLIENT Client,
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength)
{
    /* Echo the requested size, or 16x16, so cursor creation succeeds */
    BYTE Reply[32];
    WORD Width = (BodyLength >= 8) ? *(const WORD *)(Body + 4) : 16;
    WORD Height = (BodyLength >= 8) ? *(const WORD *)(Body + 6) : 16;

    RtlZeroMemory(Reply, sizeof(Reply));
    InitReplyHeader(Reply, Client, 0);
    if (Width == 0)
        Width = 16;
    if (Height == 0)
        Height = 16;
    *(WORD *)(Reply + 8) = Width;
    *(WORD *)(Reply + 10) = Height;
    SendToClient(Client, Reply, 32);
}

static
VOID
HandleGetKeyboardMapping(
    _In_ PX_CLIENT Client,
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength)
{
    /* first-keycode@0, count@1; two keysyms (base, shifted) per keycode */
    BYTE First = (BodyLength >= 1) ? Body[0] : 8;
    BYTE Count = (BodyLength >= 2) ? Body[1] : 0;
    const INT PerKeycode = 2;
    DWORD SymbolCount = (DWORD)Count * PerKeycode;
    DWORD ReplyLength = 32 + SymbolCount * 4;
    PBYTE Reply;
    INT Index;

    Reply = (PBYTE)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ReplyLength);
    if (!Reply)
        return;

    InitReplyHeader(Reply, Client, SymbolCount);
    Reply[1] = (BYTE)PerKeycode;
    for (Index = 0; Index < Count; Index++)
    {
        BYTE Keycode = (BYTE)(First + Index);

        *(DWORD *)(Reply + 32 + (Index * PerKeycode + 0) * 4) = KeycodeToKeysym(Keycode, 0);
        *(DWORD *)(Reply + 32 + (Index * PerKeycode + 1) * 4) = KeycodeToKeysym(Keycode, 1);
    }
    SendToClient(Client, Reply, ReplyLength);
    HeapFree(GetProcessHeap(), 0, Reply);
}

static
VOID
HandleGetModifierMapping(
    _In_ PX_CLIENT Client)
{
    /* Shift, Lock, Control, Mod1..Mod5 with two keycodes each; keycodes are VK codes */
    const INT PerModifier = 2;
    DWORD ReplyLength = 32 + 8 * PerModifier;
    BYTE Reply[32 + 8 * 2];

    RtlZeroMemory(Reply, sizeof(Reply));
    InitReplyHeader(Reply, Client, 2 * PerModifier);
    Reply[1] = (BYTE)PerModifier;
    Reply[32 + 0 * PerModifier] = 0x10;     /* Shift: VK_SHIFT */
    Reply[32 + 1 * PerModifier] = 0x14;     /* Lock: VK_CAPITAL */
    Reply[32 + 2 * PerModifier] = 0x11;     /* Control: VK_CONTROL */
    Reply[32 + 3 * PerModifier] = 0x12;     /* Mod1: VK_MENU */
    SendToClient(Client, Reply, ReplyLength);
}

static
VOID
HandleGetSelectionOwner(
    _In_ PX_CLIENT Client,
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength)
{
    BYTE Reply[32];
    XID Owner = (BodyLength >= 4) ? GetSelectionOwnerAtom(*(const DWORD *)(Body + 0)) : 0;

    RtlZeroMemory(Reply, sizeof(Reply));
    InitReplyHeader(Reply, Client, 0);
    *(DWORD *)(Reply + 8) = Owner;
    SendToClient(Client, Reply, 32);
}

/**
 * @brief ConvertSelection. With an owner, forwards SelectionRequest to the owner's client;
 *        otherwise refuses with a SelectionNotify whose property is None.
 */
static
VOID
HandleConvertSelection(
    _In_ PX_CLIENT Client,
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength)
{
    /* requestor@0 selection@4 target@8 property@12 time@16 */
    XID Owner = (BodyLength >= 20) ? GetSelectionOwnerAtom(*(const DWORD *)(Body + 4)) : 0;
    PX_WINDOW OwnerWindow = Owner ? FindWindowById(Owner) : NULL;
    BYTE Event[32];

    RtlZeroMemory(Event, sizeof(Event));
    if (OwnerWindow && OwnerWindow->Owner)
    {
        Event[0] = SelectionRequest;
        Event[2] = (BYTE)(OwnerWindow->Owner->Sequence & 0xFF);
        Event[3] = (BYTE)((OwnerWindow->Owner->Sequence >> 8) & 0xFF);
        *(DWORD *)(Event + 4) = g_ServerTime++;
        *(DWORD *)(Event + 8) = (DWORD)Owner;
        *(DWORD *)(Event + 12) = *(const DWORD *)(Body + 0);
        *(DWORD *)(Event + 16) = *(const DWORD *)(Body + 4);
        *(DWORD *)(Event + 20) = *(const DWORD *)(Body + 8);
        *(DWORD *)(Event + 24) = *(const DWORD *)(Body + 12);
        SendToClient(OwnerWindow->Owner, Event, 32);
        return;
    }

    Event[0] = SelectionNotify;
    Event[2] = (BYTE)(Client->Sequence & 0xFF);
    Event[3] = (BYTE)((Client->Sequence >> 8) & 0xFF);
    if (BodyLength >= 20)
    {
        *(DWORD *)(Event + 4) = *(const DWORD *)(Body + 16);
        *(DWORD *)(Event + 8) = *(const DWORD *)(Body + 0);
        *(DWORD *)(Event + 12) = *(const DWORD *)(Body + 4);
        *(DWORD *)(Event + 16) = *(const DWORD *)(Body + 8);
    }
    *(DWORD *)(Event + 20) = 0;     /* property None */
    SendToClient(Client, Event, 32);
}

static
VOID
HandleQueryFont(
    _In_ PX_CLIENT Client)
{
    BYTE Reply[60];

    RtlZeroMemory(Reply, sizeof(Reply));
    Reply[0] = 1;
    Reply[2] = (BYTE)(Client->Sequence & 0xFF);
    Reply[3] = (BYTE)((Client->Sequence >> 8) & 0xFF);
    *(DWORD *)(Reply + 4) = 7;

    /* Fixed font: min and max bounds are identical (rbearing, width, ascent, descent) */
    *(WORD *)(Reply + 10) = (WORD)g_FontWidth;
    *(WORD *)(Reply + 12) = (WORD)g_FontWidth;
    *(WORD *)(Reply + 14) = (WORD)g_FontAscent;
    *(WORD *)(Reply + 16) = (WORD)g_FontDescent;
    *(WORD *)(Reply + 26) = (WORD)g_FontWidth;
    *(WORD *)(Reply + 28) = (WORD)g_FontWidth;
    *(WORD *)(Reply + 30) = (WORD)g_FontAscent;
    *(WORD *)(Reply + 32) = (WORD)g_FontDescent;
    *(WORD *)(Reply + 40) = 32;
    *(WORD *)(Reply + 42) = 255;
    *(WORD *)(Reply + 44) = 32;
    Reply[51] = 1;
    *(WORD *)(Reply + 52) = (WORD)g_FontAscent;
    *(WORD *)(Reply + 54) = (WORD)g_FontDescent;
    SendToClient(Client, Reply, 60);
}

static
VOID
HandleListFonts(
    _In_ PX_CLIENT Client)
{
    /* Every pattern matches the single fixed font, reported with a full XLFD name */
    static const CHAR FontName[] = "-misc-fixed-medium-r-normal--13-120-75-75-c-60-iso8859-1";
    BYTE Reply[32 + 60];
    INT NameLength = (INT)(sizeof(FontName) - 1);
    INT Padded = (1 + NameLength + 3) & ~3;

    RtlZeroMemory(Reply, sizeof(Reply));
    InitReplyHeader(Reply, Client, Padded / 4);
    *(WORD *)(Reply + 8) = 1;
    Reply[32] = (BYTE)NameLength;
    RtlCopyMemory(Reply + 33, FontName, NameLength);
    SendToClient(Client, Reply, 32 + Padded);
}

static
VOID
HandleAllocColor(
    _In_ PX_CLIENT Client,
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength)
{
    BYTE Reply[32];

    InitReplyHeader(Reply, Client, 0);
    if (BodyLength >= 10)
    {
        WORD Red = *(const WORD *)(Body + 4);
        WORD Green = *(const WORD *)(Body + 6);
        WORD Blue = *(const WORD *)(Body + 8);

        *(WORD *)(Reply + 8) = Red;
        *(WORD *)(Reply + 10) = Green;
        *(WORD *)(Reply + 12) = Blue;
        *(DWORD *)(Reply + 16) = ((DWORD)(Red >> 8) << 16) | ((DWORD)(Green >> 8) << 8) | (Blue >> 8);
    }
    SendToClient(Client, Reply, 32);
}

/**
 * @brief AllocNamedColor and LookupColor: cmap@0, name length@4, name@8.
 */
static
VOID
HandleNamedColor(
    _In_ PX_CLIENT Client,
    _In_ BOOL Allocate,
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength)
{
    BYTE Reply[32];
    WORD Red = 0xFFFF;
    WORD Green = 0xFFFF;
    WORD Blue = 0xFFFF;

    InitReplyHeader(Reply, Client, 0);
    if (BodyLength >= 8)
    {
        INT NameLength = *(const WORD *)(Body + 4);

        if ((DWORD)NameLength > BodyLength - 8)
            NameLength = 0;
        LookupColorName((PCSTR)(Body + 8), NameLength, &Red, &Green, &Blue);
    }

    if (Allocate)
    {
        /* Pixel, then exact and screen RGB */
        *(DWORD *)(Reply + 8) = ((DWORD)(Red >> 8) << 16) | ((DWORD)(Green >> 8) << 8) | (Blue >> 8);
        *(WORD *)(Reply + 12) = Red;
        *(WORD *)(Reply + 14) = Green;
        *(WORD *)(Reply + 16) = Blue;
        *(WORD *)(Reply + 18) = Red;
        *(WORD *)(Reply + 20) = Green;
        *(WORD *)(Reply + 22) = Blue;
    }
    else
    {
        /* Exact and visual RGB */
        *(WORD *)(Reply + 8) = Red;
        *(WORD *)(Reply + 10) = Green;
        *(WORD *)(Reply + 12) = Blue;
        *(WORD *)(Reply + 14) = Red;
        *(WORD *)(Reply + 16) = Green;
        *(WORD *)(Reply + 18) = Blue;
    }
    SendToClient(Client, Reply, 32);
}

static
VOID
HandleQueryColors(
    _In_ PX_CLIENT Client,
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength)
{
    /* Pixels are literal 0x00RRGGBB values, so each one is unpacked directly */
    INT Count = (BodyLength >= 4) ? (INT)((BodyLength - 4) / 4) : 0;
    INT Index;
    PBYTE Reply;
    DWORD ReplyLength = 32 + (DWORD)Count * 8;

    Reply = (PBYTE)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ReplyLength);
    if (!Reply)
        return;

    InitReplyHeader(Reply, Client, (Count * 8) / 4);
    *(WORD *)(Reply + 8) = (WORD)Count;
    for (Index = 0; Index < Count; Index++)
    {
        DWORD Pixel = *(const DWORD *)(Body + 4 + Index * 4);
        BYTE Red = (BYTE)(Pixel >> 16);
        BYTE Green = (BYTE)(Pixel >> 8);
        BYTE Blue = (BYTE)Pixel;

        *(WORD *)(Reply + 32 + Index * 8 + 0) = (WORD)(Red << 8 | Red);
        *(WORD *)(Reply + 32 + Index * 8 + 2) = (WORD)(Green << 8 | Green);
        *(WORD *)(Reply + 32 + Index * 8 + 4) = (WORD)(Blue << 8 | Blue);
    }
    SendToClient(Client, Reply, ReplyLength);
    HeapFree(GetProcessHeap(), 0, Reply);
}

static
VOID
HandleCreateGc(
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength)
{
    XID GcId;
    PX_GCONTEXT Gc;

    if (BodyLength < 12)
        return;

    GcId = *(const DWORD *)(Body + 0);
    Gc = FindGContext(GcId);
    if (!Gc)
    {
        Gc = (PX_GCONTEXT)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Gc));
        if (Gc)
        {
            /* X defaults, except the background is white rather than pixel 1 */
            Gc->Id = GcId;
            Gc->Function = GXcopy;
            Gc->PlaneMask = 0xFFFFFFFF;
            Gc->Background = WHITE_PIXEL;
            Gc->CapStyle = 1;           /* Butt */
            Gc->GraphicsExposures = 1;
            Gc->ArcMode = 1;            /* PieSlice */
            Gc->Dashes[0] = 4;
            Gc->Dashes[1] = 4;
            Gc->DashCount = 2;
            Gc->Next = g_GcList;
            g_GcList = Gc;
        }
    }

    if (Gc)
        ParseGcValues(Gc, *(const DWORD *)(Body + 8), (const DWORD *)(Body + 12), (BodyLength - 12) / 4);
}

static
VOID
HandleCopyGc(
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength)
{
    PX_GCONTEXT Source;
    PX_GCONTEXT Dest;
    DWORD Mask;

    if (BodyLength < 12)
        return;

    Source = FindGContext(*(const DWORD *)(Body + 0));
    Dest = FindGContext(*(const DWORD *)(Body + 4));
    Mask = *(const DWORD *)(Body + 8);
    if (!Source || !Dest)
        return;

    if (Mask & GCFunction)
        Dest->Function = Source->Function;
    if (Mask & GCPlaneMask)
        Dest->PlaneMask = Source->PlaneMask;
    if (Mask & GCForeground)
        Dest->Foreground = Source->Foreground;
    if (Mask & GCBackground)
        Dest->Background = Source->Background;
    if (Mask & GCLineWidth)
        Dest->LineWidth = Source->LineWidth;
    if (Mask & GCLineStyle)
        Dest->LineStyle = Source->LineStyle;
    if (Mask & GCCapStyle)
        Dest->CapStyle = Source->CapStyle;
    if (Mask & GCJoinStyle)
        Dest->JoinStyle = Source->JoinStyle;
    if (Mask & GCFillStyle)
        Dest->FillStyle = Source->FillStyle;
    if (Mask & GCFillRule)
        Dest->FillRule = Source->FillRule;
    if (Mask & GCTile)
        Dest->Tile = Source->Tile;
    if (Mask & GCStipple)
        Dest->Stipple = Source->Stipple;
    if (Mask & GCTileStipXOrigin)
        Dest->TileStipXOrigin = Source->TileStipXOrigin;
    if (Mask & GCTileStipYOrigin)
        Dest->TileStipYOrigin = Source->TileStipYOrigin;
    if (Mask & GCFont)
        Dest->Font = Source->Font;
    if (Mask & GCSubwindowMode)
        Dest->SubwindowMode = Source->SubwindowMode;
    if (Mask & GCGraphicsExposures)
        Dest->GraphicsExposures = Source->GraphicsExposures;
    if (Mask & GCClipXOrigin)
        Dest->ClipXOrigin = Source->ClipXOrigin;
    if (Mask & GCClipYOrigin)
        Dest->ClipYOrigin = Source->ClipYOrigin;

    if (Mask & GCClipMask)
    {
        Dest->ClipMask = Source->ClipMask;
        if (Dest->ClipRects)
        {
            HeapFree(GetProcessHeap(), 0, Dest->ClipRects);
            Dest->ClipRects = NULL;
        }
        Dest->ClipRectCount = 0;

        if (Source->ClipRects && Source->ClipRectCount > 0)
        {
            Dest->ClipRects = (PRECT)HeapAlloc(GetProcessHeap(),
                                               0,
                                               Source->ClipRectCount * sizeof(*Dest->ClipRects));
            if (Dest->ClipRects)
            {
                RtlCopyMemory(Dest->ClipRects,
                              Source->ClipRects,
                              Source->ClipRectCount * sizeof(*Dest->ClipRects));
                Dest->ClipRectCount = Source->ClipRectCount;
            }
        }
    }

    if (Mask & GCDashOffset)
        Dest->DashOffset = Source->DashOffset;

    if (Mask & GCDashList)
    {
        RtlCopyMemory(Dest->Dashes, Source->Dashes, sizeof(Dest->Dashes));
        Dest->DashCount = Source->DashCount;
    }

    if (Mask & GCArcMode)
        Dest->ArcMode = Source->ArcMode;
}

static
VOID
HandleSetDashes(
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength)
{
    PX_GCONTEXT Gc;
    WORD DashCount;

    /* gc@0 dash-offset@4 count@6 dashes@8 */
    if (BodyLength < 8)
        return;

    Gc = FindGContext(*(const DWORD *)(Body + 0));
    DashCount = *(const WORD *)(Body + 6);
    if (Gc && (DWORD)(8 + DashCount) <= BodyLength)
    {
        DWORD Index;
        DWORD Count = DashCount < GC_MAX_DASHES ? DashCount : GC_MAX_DASHES;

        Gc->DashOffset = *(const WORD *)(Body + 4);
        for (Index = 0; Index < Count; Index++)
            Gc->Dashes[Index] = Body[8 + Index];
        Gc->DashCount = Count;

        /* Dashes imply OnOffDash rendering */
        if (Count)
            Gc->LineStyle = 1;
    }
}

static
VOID
HandleSetClipRectangles(
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength)
{
    PX_GCONTEXT Gc;
    DWORD Count;
    DWORD Index;

    /* ordering in the header data byte, gc@0 clip-x@4 clip-y@6 rects@8 */
    if (BodyLength < 8)
        return;

    Gc = FindGContext(*(const DWORD *)(Body + 0));
    Count = (BodyLength - 8) / 8;
    if (!Gc)
        return;

    Gc->ClipXOrigin = *(const SHORT *)(Body + 4);
    Gc->ClipYOrigin = *(const SHORT *)(Body + 6);
    Gc->ClipMask = 0;
    if (Gc->ClipRects)
    {
        HeapFree(GetProcessHeap(), 0, Gc->ClipRects);
        Gc->ClipRects = NULL;
    }
    Gc->ClipRectCount = 0;

    if (Count > 0)
    {
        Gc->ClipRects = (PRECT)HeapAlloc(GetProcessHeap(), 0, Count * sizeof(*Gc->ClipRects));
        if (Gc->ClipRects)
        {
            for (Index = 0; Index < Count; Index++)
            {
                const BYTE *RectData = Body + 8 + Index * 8;

                Gc->ClipRects[Index].left = *(const SHORT *)(RectData + 0);
                Gc->ClipRects[Index].top = *(const SHORT *)(RectData + 2);
                Gc->ClipRects[Index].right = Gc->ClipRects[Index].left + *(const WORD *)(RectData + 4);
                Gc->ClipRects[Index].bottom = Gc->ClipRects[Index].top + *(const WORD *)(RectData + 6);
            }
            Gc->ClipRectCount = (INT)Count;
        }
    }
    else
    {
        /* An empty list clips everything, modeled as a single empty rect */
        Gc->ClipRects = (PRECT)HeapAlloc(GetProcessHeap(), 0, sizeof(*Gc->ClipRects));
        if (Gc->ClipRects)
        {
            Gc->ClipRects[0].left = Gc->ClipRects[0].top = 0;
            Gc->ClipRects[0].right = Gc->ClipRects[0].bottom = 0;
            Gc->ClipRectCount = 1;
        }
    }
}

static
VOID
HandleFreeGc(
    _In_reads_bytes_(4) const BYTE *Body)
{
    XID GcId = *(const DWORD *)Body;
    PX_GCONTEXT *Link;

    for (Link = &g_GcList; *Link; Link = &(*Link)->Next)
    {
        if ((*Link)->Id == GcId)
        {
            PX_GCONTEXT Gc = *Link;

            *Link = Gc->Next;
            if (Gc->ClipRects)
                HeapFree(GetProcessHeap(), 0, Gc->ClipRects);
            HeapFree(GetProcessHeap(), 0, Gc);
            break;
        }
    }
}

/**
 * @brief CreatePixmap. Depth 1 creates a monochrome bitmap so pattern brushes and mono
 *        blits get GDI's text/background color mapping.
 */
static
VOID
HandleCreatePixmap(
    _In_ PCX_REQ_HEAD Header,
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength)
{
    /* depth in the header data byte, pid@0 drawable@4 width@8 height@10 */
    WORD Width;
    WORD Height;
    PX_PIXMAP Pixmap;
    HDC WindowDc;

    if (BodyLength < 12)
        return;

    Width = *(const WORD *)(Body + 8);
    Height = *(const WORD *)(Body + 10);
    Pixmap = (PX_PIXMAP)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Pixmap));
    if (!Pixmap)
        return;

    WindowDc = GetDC(g_Hwnd);
    Pixmap->Id = *(const DWORD *)(Body + 0);
    Pixmap->Width = Width;
    Pixmap->Height = Height;
    Pixmap->Depth = (Header->data == 1) ? 1 : 32;
    Pixmap->Dc = CreateCompatibleDC(WindowDc);
    if (Pixmap->Depth == 1)
    {
        Pixmap->Bitmap = CreateBitmap(Width ? Width : 1, Height ? Height : 1, 1, 1, NULL);
        Pixmap->Bits = NULL;
    }
    else
    {
        BITMAPINFO Bmi;

        RtlZeroMemory(&Bmi, sizeof(Bmi));
        Bmi.bmiHeader.biSize = sizeof(Bmi.bmiHeader);
        Bmi.bmiHeader.biWidth = Width ? Width : 1;
        Bmi.bmiHeader.biHeight = -(LONG)(Height ? Height : 1);
        Bmi.bmiHeader.biPlanes = 1;
        Bmi.bmiHeader.biBitCount = 32;
        Bmi.bmiHeader.biCompression = BI_RGB;
        Pixmap->Bitmap = CreateDIBSection(WindowDc, &Bmi, DIB_RGB_COLORS, &Pixmap->Bits, NULL, 0);
    }
    SelectObject(Pixmap->Dc, Pixmap->Bitmap);
    SelectObject(Pixmap->Dc, GetStockObject(ANSI_FIXED_FONT));
    ReleaseDC(g_Hwnd, WindowDc);
    Pixmap->Next = g_Pixmaps;
    g_Pixmaps = Pixmap;
}

static
VOID
HandleFreePixmap(
    _In_reads_bytes_(4) const BYTE *Body)
{
    XID PixmapId = *(const DWORD *)Body;
    PX_PIXMAP *Link;

    for (Link = &g_Pixmaps; *Link; Link = &(*Link)->Next)
    {
        if ((*Link)->Id == PixmapId)
        {
            PX_PIXMAP Pixmap = *Link;

            *Link = Pixmap->Next;
            DeleteDC(Pixmap->Dc);
            DeleteObject(Pixmap->Bitmap);
            HeapFree(GetProcessHeap(), 0, Pixmap);
            break;
        }
    }
}

static
VOID
HandleCopyArea(
    _In_ PX_CLIENT Client,
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength)
{
    XID Dest;
    PX_GCONTEXT Gc;

    /* src@0 dst@4 gc@8 src-x@12 src-y@14 dst-x@16 dst-y@18 width@20 height@22 */
    if (BodyLength < 24)
        return;

    Dest = *(const DWORD *)(Body + 4);
    Gc = FindGContext(*(const DWORD *)(Body + 8));
    DoCopyArea(*(const DWORD *)(Body + 0),
               Dest,
               Gc,
               *(const SHORT *)(Body + 12),
               *(const SHORT *)(Body + 14),
               *(const WORD *)(Body + 20),
               *(const WORD *)(Body + 22),
               *(const SHORT *)(Body + 16),
               *(const SHORT *)(Body + 18));

    /*
     * With graphics-exposures set, CopyArea must be answered with NoExpose or GraphicsExpose.
     * Source pixels are always available in the screen DIB, so NoExpose is always correct.
     */
    if (!Gc || Gc->GraphicsExposures)
    {
        BYTE Event[32];

        RtlZeroMemory(Event, sizeof(Event));
        Event[0] = NoExpose;
        Event[2] = (BYTE)(Client->Sequence & 0xFF);
        Event[3] = (BYTE)((Client->Sequence >> 8) & 0xFF);
        *(DWORD *)(Event + 4) = Dest;
        *(WORD *)(Event + 8) = 0;       /* minor opcode */
        Event[10] = X_CopyArea;         /* major opcode */
        SendToClient(Client, Event, 32);
    }
}

/**
 * @brief Logs text drawn into windows owned by the window manager.
 */
static
VOID
LogWmText(
    _In_ XID Drawable,
    _In_z_ PCSTR Tag,
    _In_ INT Length)
{
    PX_WINDOW Window = FindWindowById(Drawable);

    if (Window && Window->Owner && FindRedirectClient(g_Root, NULL) == Window->Owner)
    {
        if (Length >= 0)
            XWmLog("%s %lx n=%d", Tag, (ULONG)Window->Id, Length);
        else
            XWmLog("%s %lx", Tag, (ULONG)Drawable);
    }
}

static
VOID
HandleImageText8(
    _In_ PCX_REQ_HEAD Header,
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength)
{
    X_TARGET Target;
    INT Length = Header->data;

    /* drawable@0 gc@4 x@8 y@10 string@12 */
    if (BodyLength < 12)
        return;

    LogWmText(*(const DWORD *)(Body + 0), "wmitxt", (INT)Header->data);
    ResolveDrawable(*(const DWORD *)(Body + 0), &Target);
    if (Target.Valid && (DWORD)(12 + Length) <= BodyLength)
    {
        DrawImageText(&Target,
                      FindGContext(*(const DWORD *)(Body + 4)),
                      *(const SHORT *)(Body + 8),
                      *(const SHORT *)(Body + 10),
                      (PCSTR)(Body + 12),
                      Length);
    }
}

static
VOID
HandleImageText16(
    _In_ PCX_REQ_HEAD Header,
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength)
{
    X_TARGET Target;
    INT Length = Header->data;
    CHAR Buffer[256];
    INT Index;

    /* Big-endian CHAR2B characters; only the low 256 code points are drawable */
    if (BodyLength < 12)
        return;

    ResolveDrawable(*(const DWORD *)(Body + 0), &Target);
    if (!Target.Valid)
        return;

    if ((DWORD)(12 + Length * 2) <= BodyLength && Length > 0 && Length <= 255)
    {
        for (Index = 0; Index < Length; Index++)
        {
            WORD Char = (WORD)((Body[12 + Index * 2] << 8) | Body[12 + Index * 2 + 1]);

            Buffer[Index] = (Char < 256) ? (CHAR)Char : '?';
        }
        DrawImageText(&Target,
                      FindGContext(*(const DWORD *)(Body + 4)),
                      *(const SHORT *)(Body + 8),
                      *(const SHORT *)(Body + 10),
                      Buffer,
                      Length);
    }
}

/**
 * @brief PolyText8 and PolyText16. Items are [length][delta][string] or a font shift
 *        [255][font:4], which is skipped. Glyphs are drawn transparently.
 */
static
VOID
HandlePolyText(
    _In_ BOOL Wide,
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength)
{
    XID Drawable;
    PX_GCONTEXT Gc;
    SHORT Y;
    INT CurrentX;
    const BYTE *Item;
    const BYTE *End;
    INT CharSize = Wide ? 2 : 1;

    /* drawable@0 gc@4 x@8 y@10 items@12 */
    if (BodyLength < 12)
        return;

    Drawable = *(const DWORD *)(Body + 0);
    if (!Wide)
        LogWmText(Drawable, "wmptxt", -1);

    Gc = FindGContext(*(const DWORD *)(Body + 4));
    Y = *(const SHORT *)(Body + 10);
    CurrentX = *(const SHORT *)(Body + 8);
    Item = Body + 12;
    End = Body + BodyLength;

    while (Item + 2 <= End)
    {
        BYTE Length = Item[0];
        X_TARGET Target;
        CHAR Buffer[256];
        PCSTR String;
        INT Index;

        /* Font shift */
        if (Length == 255)
        {
            Item += 5;
            continue;
        }

        /* Padding or end of list */
        if (Length == 0)
            break;
        if (Item + 2 + Length * CharSize > End)
            break;

        CurrentX += (signed char)Item[1];
        if (Wide)
        {
            for (Index = 0; Index < Length; Index++)
            {
                WORD Char = (WORD)((Item[2 + Index * 2] << 8) | Item[2 + Index * 2 + 1]);

                Buffer[Index] = (Char < 256) ? (CHAR)Char : '?';
            }
            String = Buffer;
        }
        else
        {
            String = (PCSTR)(Item + 2);
        }

        ResolveDrawable(Drawable, &Target);
        if (Target.Valid)
            DrawTextGeneric(&Target, Gc, (SHORT)CurrentX, Y, String, Length, FALSE);
        CurrentX += Length * g_FontWidth;
        Item += 2 + Length * CharSize;
    }
}

static
VOID
HandlePutImage(
    _In_ PCX_REQ_HEAD Header,
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength)
{
    X_TARGET Target;

    /* format in the header data byte, drawable@0 gc@4 width@8 height@10 dst-x@12 dst-y@14
       left-pad@16 depth@17 data@20 */
    if (BodyLength < 20)
        return;

    ResolveDrawable(*(const DWORD *)(Body + 0), &Target);
    if (Target.Valid)
    {
        DrawPutImage(&Target,
                     FindGContext(*(const DWORD *)(Body + 4)),
                     Header->data,
                     *(const SHORT *)(Body + 12),
                     *(const SHORT *)(Body + 14),
                     *(const WORD *)(Body + 8),
                     *(const WORD *)(Body + 10),
                     Body[16],
                     Body[17],
                     Body + 20,
                     BodyLength - 20);
    }
}

/**
 * @brief Handles the PolyX drawing requests that share the drawable@0 gc@4 items@N layout.
 */
static
VOID
HandlePolyDraw(
    _In_ PCX_REQ_HEAD Header,
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength)
{
    X_TARGET Target;
    PX_GCONTEXT Gc;
    DWORD MinLength = (Header->opcode == X_FillPoly) ? 12 : 8;

    if (BodyLength < MinLength)
        return;

    ResolveDrawable(*(const DWORD *)(Body + 0), &Target);
    if (!Target.Valid)
        return;

    Gc = FindGContext(*(const DWORD *)(Body + 4));
    switch (Header->opcode)
    {
        case X_PolyFillRectangle:
            DrawFillRects(&Target, Gc, (PCX_RECT)(Body + 8), (BodyLength - 8) / 8);
            break;

        case X_PolyRectangle:
            DrawFrameRects(&Target, Gc, (PCX_RECT)(Body + 8), (BodyLength - 8) / 8);
            break;

        case X_PolySegment:
            DrawSegments(&Target, Gc, (PCX_SEGMENT)(Body + 8), (BodyLength - 8) / 8);
            break;

        case X_PolyLine:
            DrawPolyline(&Target, Gc, (PCX_POINT)(Body + 8), (BodyLength - 8) / 4, Header->data);
            break;

        case X_FillPoly:
            DrawFillPoly(&Target, Gc, (PCX_POINT)(Body + 12), (BodyLength - 12) / 4);
            break;

        case X_PolyFillArc:
            DrawFillArcs(&Target, Gc, (PCX_ARC)(Body + 8), (BodyLength - 8) / 12);
            break;

        case X_PolyArc:
            DrawArcs(&Target, Gc, (PCX_ARC)(Body + 8), (BodyLength - 8) / 12);
            break;

        case X_PolyPoint:
            DrawPoints(&Target, Gc, (PCX_POINT)(Body + 8), (BodyLength - 8) / 4, Header->data);
            break;
    }
}

static
VOID
DispatchRequest(
    _In_ PX_CLIENT Client,
    _In_ PCX_REQ_HEAD Header,
    _In_reads_bytes_(BodyLength) const BYTE *Body,
    _In_ DWORD BodyLength)
{
    XTrace("psxx11: c%lu req op=%u len=%lu\n", Client->Id, Header->opcode, BodyLength);

    switch (Header->opcode)
    {
        case X_CreateWindow:
            HandleCreateWindow(Client, Body, BodyLength);
            break;

        case X_ChangeWindowAttributes:
        {
            PX_WINDOW Window;

            if (BodyLength < 8)
                break;

            Window = FindWindowById(*(const DWORD *)(Body + 0));
            if (Window)
            {
                ApplyWindowAttributes(Client,
                                      Window,
                                      *(const DWORD *)(Body + 4),
                                      (const DWORD *)(Body + 8),
                                      (BodyLength - 8) / 4);
            }
            break;
        }

        case X_GetWindowAttributes:
            HandleGetWindowAttributes(Client, Body);
            break;

        case X_DestroyWindow:
            HandleDestroyWindow(Body);
            break;

        case X_ReparentWindow:
            HandleReparentWindow(Body, BodyLength);
            break;

        case X_MapWindow:
        {
            PX_WINDOW Window = FindWindowById(*(const DWORD *)Body);

            if (Window)
                DoMapWindow(Client, Window);
            break;
        }

        case X_MapSubwindows:
        {
            /* Map every child; DoMapWindow honors redirect per child */
            PX_WINDOW Window = FindWindowById(*(const DWORD *)Body);
            PX_WINDOW Child;

            if (Window)
            {
                for (Child = Window->FirstChild; Child; Child = Child->NextSibling)
                    DoMapWindow(Client, Child);
            }
            break;
        }

        case X_UnmapWindow:
        {
            PX_WINDOW Window = FindWindowById(*(const DWORD *)Body);

            if (Window && Window->Mapped)
            {
                Window->Mapped = FALSE;
                SendUnmapNotify(Window);
                RepaintScreen();
            }
            break;
        }

        case X_UnmapSubwindows:
        {
            PX_WINDOW Window = FindWindowById(*(const DWORD *)Body);
            PX_WINDOW Child;

            if (!Window)
                break;

            for (Child = Window->FirstChild; Child; Child = Child->NextSibling)
            {
                if (Child->Mapped)
                {
                    Child->Mapped = FALSE;
                    SendUnmapNotify(Child);
                }
            }
            RepaintScreen();
            break;
        }

        case X_ConfigureWindow:
        {
            PX_WINDOW Window;

            if (BodyLength < 8)
                break;

            Window = FindWindowById(*(const DWORD *)(Body + 0));
            if (Window)
            {
                DoConfigureWindow(Client,
                                  Window,
                                  *(const WORD *)(Body + 4),
                                  (const DWORD *)(Body + 8),
                                  (BodyLength - 8) / 4);
            }
            break;
        }

        case X_GetGeometry:
            HandleGetGeometry(Client, Body);
            break;

        case X_QueryTree:
            HandleQueryTree(Client, Body);
            break;

        case X_InternAtom:
            HandleInternAtom(Client, Header, Body, BodyLength);
            break;

        case X_GetAtomName:
            HandleGetAtomName(Client, Body, BodyLength);
            break;

        case X_ChangeProperty:
            HandleChangeProperty(Header, Body, BodyLength);
            break;

        case X_DeleteProperty:
            HandleDeleteProperty(Body, BodyLength);
            break;

        case X_GetProperty:
            HandleGetProperty(Client, Header, Body, BodyLength);
            break;

        case X_ListProperties:
            HandleListProperties(Client, Body, BodyLength);
            break;

        case X_GetInputFocus:
        {
            BYTE Reply[32];

            InitReplyHeader(Reply, Client, 0);
            Reply[1] = 0;                       /* revert-to */
            *(DWORD *)(Reply + 8) = ID_ROOT;    /* focus */
            SendToClient(Client, Reply, 32);
            break;
        }

        case X_QueryPointer:
        {
            BYTE Reply[32];

            InitReplyHeader(Reply, Client, 0);
            Reply[1] = 1;                       /* same-screen */
            *(DWORD *)(Reply + 8) = ID_ROOT;
            *(WORD *)(Reply + 16) = (WORD)g_PointerX;
            *(WORD *)(Reply + 18) = (WORD)g_PointerY;
            *(WORD *)(Reply + 20) = (WORD)g_PointerX;
            *(WORD *)(Reply + 22) = (WORD)g_PointerY;
            SendToClient(Client, Reply, 32);
            break;
        }

        case X_TranslateCoordinates:
        {
            BYTE Reply[32];

            InitReplyHeader(Reply, Client, 0);
            Reply[1] = 1;
            if (BodyLength >= 12)
            {
                *(WORD *)(Reply + 12) = *(const WORD *)(Body + 8);
                *(WORD *)(Reply + 14) = *(const WORD *)(Body + 10);
            }
            SendToClient(Client, Reply, 32);
            break;
        }

        case X_GrabPointer:
        {
            BYTE Reply[32];

            InitReplyHeader(Reply, Client, 0);
            Reply[1] = 0;                       /* Success */
            g_PointerGrab = Client;
            g_PointerGrabWindow = (BodyLength >= 4) ? *(const DWORD *)(Body + 0) : ID_ROOT;
            g_PointerGrabExplicit = TRUE;
            SendToClient(Client, Reply, 32);
            break;
        }

        case X_GrabKey:
            /* GrabKey has no reply; key grabs are not modeled */
            break;

        case X_GrabKeyboard:
        {
            BYTE Reply[32];

            InitReplyHeader(Reply, Client, 0);
            Reply[1] = 0;                       /* Success */
            g_KeyboardGrab = Client;
            g_KeyboardGrabWindow = (BodyLength >= 4) ? *(const DWORD *)(Body + 0) : ID_ROOT;
            SendToClient(Client, Reply, 32);
            break;
        }

        case X_UngrabKeyboard:
            g_KeyboardGrab = NULL;
            break;

        case X_UngrabPointer:
            g_PointerGrab = NULL;
            g_PointerGrabExplicit = FALSE;
            break;

        case X_GrabButton:
            if (BodyLength >= 18 && g_ButtonGrabCount < MAX_GRABS)
            {
                g_ButtonGrabs[g_ButtonGrabCount].Client = Client;
                g_ButtonGrabs[g_ButtonGrabCount].Window = *(const DWORD *)(Body + 0);
                g_ButtonGrabs[g_ButtonGrabCount].Button = Body[16];
                g_ButtonGrabCount++;
            }
            break;

        case X_UngrabButton:
            HandleUngrabButton(Client, Header, Body, BodyLength);
            break;

        case X_QueryBestSize:
            HandleQueryBestSize(Client, Body, BodyLength);
            break;

        case X_GetKeyboardMapping:
            HandleGetKeyboardMapping(Client, Body, BodyLength);
            break;

        case X_GetModifierMapping:
            HandleGetModifierMapping(Client);
            break;

        case X_SetSelectionOwner:
            /* owner@0 selection@4 time@8 */
            if (BodyLength >= 8)
                SetSelectionOwnerAtom(*(const DWORD *)(Body + 4), *(const DWORD *)(Body + 0));
            break;

        case X_GetSelectionOwner:
            HandleGetSelectionOwner(Client, Body, BodyLength);
            break;

        case X_ConvertSelection:
            HandleConvertSelection(Client, Body, BodyLength);
            break;

        case X_QueryFont:
            HandleQueryFont(Client);
            break;

        case X_ListFonts:
            HandleListFonts(Client);
            break;

        case X_AllocColor:
            HandleAllocColor(Client, Body, BodyLength);
            break;

        case X_AllocNamedColor:
            HandleNamedColor(Client, TRUE, Body, BodyLength);
            break;

        case X_QueryColors:
            HandleQueryColors(Client, Body, BodyLength);
            break;

        case X_LookupColor:
            HandleNamedColor(Client, FALSE, Body, BodyLength);
            break;

        case X_QueryExtension:
        {
            /* present = False */
            BYTE Reply[32];

            InitReplyHeader(Reply, Client, 0);
            SendToClient(Client, Reply, 32);
            break;
        }

        case X_CreateGC:
            HandleCreateGc(Body, BodyLength);
            break;

        case X_ChangeGC:
        {
            PX_GCONTEXT Gc;

            if (BodyLength < 8)
                break;

            Gc = FindGContext(*(const DWORD *)(Body + 0));
            if (Gc)
                ParseGcValues(Gc, *(const DWORD *)(Body + 4), (const DWORD *)(Body + 8), (BodyLength - 8) / 4);
            break;
        }

        case X_CopyGC:
            HandleCopyGc(Body, BodyLength);
            break;

        case X_SetDashes:
            HandleSetDashes(Body, BodyLength);
            break;

        case X_SetClipRectangles:
            HandleSetClipRectangles(Body, BodyLength);
            break;

        case X_FreeGC:
            HandleFreeGc(Body);
            break;

        case X_CreatePixmap:
            HandleCreatePixmap(Header, Body, BodyLength);
            break;

        case X_FreePixmap:
            HandleFreePixmap(Body);
            break;

        case X_ClearArea:
        {
            PX_WINDOW Window;

            if (BodyLength < 12)
                break;

            Window = FindWindowById(*(const DWORD *)(Body + 0));
            if (Window)
            {
                DoClearArea(Window,
                            *(const SHORT *)(Body + 4),
                            *(const SHORT *)(Body + 6),
                            *(const WORD *)(Body + 8),
                            *(const WORD *)(Body + 10));
            }
            break;
        }

        case X_CopyArea:
            HandleCopyArea(Client, Body, BodyLength);
            break;

        case X_PolyFillRectangle:
        case X_PolyRectangle:
        case X_PolySegment:
        case X_PolyLine:
        case X_FillPoly:
        case X_PolyFillArc:
        case X_PolyArc:
        case X_PolyPoint:
            HandlePolyDraw(Header, Body, BodyLength);
            break;

        case X_CopyPlane:
            /* src@0 dst@4 gc@8 src-x@12 src-y@14 dst-x@16 dst-y@18 width@20 height@22 plane@24 */
            if (BodyLength >= 28)
            {
                DoCopyPlane(*(const DWORD *)(Body + 0),
                            *(const DWORD *)(Body + 4),
                            FindGContext(*(const DWORD *)(Body + 8)),
                            *(const SHORT *)(Body + 12),
                            *(const SHORT *)(Body + 14),
                            *(const WORD *)(Body + 20),
                            *(const WORD *)(Body + 22),
                            *(const SHORT *)(Body + 16),
                            *(const SHORT *)(Body + 18));
            }
            break;

        case X_ImageText8:
            HandleImageText8(Header, Body, BodyLength);
            break;

        case X_PolyText8:
            HandlePolyText(FALSE, Body, BodyLength);
            break;

        case X_PutImage:
            HandlePutImage(Header, Body, BodyLength);
            break;

        case X_ImageText16:
            HandleImageText16(Header, Body, BodyLength);
            break;

        case X_PolyText16:
            HandlePolyText(TRUE, Body, BodyLength);
            break;

        default:
            /*
             * Requests without a reply are ignored. Unimplemented requests with a reply
             * get BadImplementation so the client reports the opcode instead of hanging.
             */
            if (IsReplyOpcode(Header->opcode))
            {
                BYTE Error[32];

                RtlZeroMemory(Error, sizeof(Error));
                Error[0] = 0;
                Error[1] = BadImplementation;
                Error[2] = (BYTE)(Client->Sequence & 0xFF);
                Error[3] = (BYTE)((Client->Sequence >> 8) & 0xFF);
                Error[10] = Header->opcode;
                SendToClient(Client, Error, 32);
                XTrace("psxx11: unimplemented reply op=%u -> BadImplementation\n", Header->opcode);
            }
            break;
    }
}

/* Client lifetime */

static
VOID
FreeClient(
    _In_ PX_CLIENT Client)
{
    PX_WINDOW Window;
    INT Index;
    INT Kept;

    /* Drop this client's selections from every window */
    for (Window = g_AllWindows; Window; Window = Window->AllNext)
    {
        for (Index = 0, Kept = 0; Index < Window->SelectorCount; Index++)
        {
            if (Window->Selectors[Index].Client != Client)
                Window->Selectors[Kept++] = Window->Selectors[Index];
        }
        Window->SelectorCount = Kept;
    }

    /* Drop this client's grabs */
    if (g_PointerGrab == Client)
    {
        g_PointerGrab = NULL;
        g_PointerGrabExplicit = FALSE;
    }
    if (g_KeyboardGrab == Client)
        g_KeyboardGrab = NULL;

    for (Index = 0, Kept = 0; Index < g_ButtonGrabCount; Index++)
    {
        if (g_ButtonGrabs[Index].Client != Client)
            g_ButtonGrabs[Kept++] = g_ButtonGrabs[Index];
    }
    g_ButtonGrabCount = Kept;

    XTrace("psxx11: client %lu gone\n", Client->Id);
    XWmLog("c%lu GONE", Client->Id);
    CancelIo(Client->Pipe);
    FlushFileBuffers(Client->Pipe);
    DisconnectNamedPipe(Client->Pipe);
    CloseHandle(Client->Pipe);
    if (Client->ReadEvent)
        CloseHandle(Client->ReadEvent);
    if (Client->WriteEvent)
        CloseHandle(Client->WriteEvent);
    if (Client->InBuffer)
        HeapFree(GetProcessHeap(), 0, Client->InBuffer);
    if (Client->OutBuffer)
        HeapFree(GetProcessHeap(), 0, Client->OutBuffer);
    HeapFree(GetProcessHeap(), 0, Client);
}

/**
 * @brief Unlinks and frees every client that died during this loop iteration.
 */
static
VOID
ReapDeadClients(VOID)
{
    PX_CLIENT *Link = &g_Clients;

    while (*Link)
    {
        PX_CLIENT Client = *Link;

        if (!Client->Alive)
        {
            *Link = Client->Next;
            FreeClient(Client);
        }
        else
        {
            Link = &Client->Next;
        }
    }
}

/* Accepting connections: one pending pipe instance with an overlapped ConnectNamedPipe */
static HANDLE g_AcceptPipe = INVALID_HANDLE_VALUE;
static OVERLAPPED g_AcceptOverlapped;
static HANDLE g_AcceptEvent;

static
VOID
StartAccept(VOID);

/**
 * @brief Turns the connected accept pipe into a client and starts its first read.
 */
static
VOID
FinalizeClient(VOID)
{
    PX_CLIENT Client;

    Client = (PX_CLIENT)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Client));
    if (!Client)
    {
        CloseHandle(g_AcceptPipe);
        g_AcceptPipe = INVALID_HANDLE_VALUE;
        return;
    }

    Client->Pipe = g_AcceptPipe;
    Client->Id = ++g_ClientSeq;
    Client->Alive = TRUE;
    Client->State = CS_HANDSHAKE;
    Client->IdBase = ID_CLIENT_BASE + (Client->Id - 1) * ID_CLIENT_STRIDE;
    Client->IdMask = ID_CLIENT_MASK;
    Client->ReadEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Client->WriteEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Client->Next = g_Clients;
    g_Clients = Client;
    g_AcceptPipe = INVALID_HANDLE_VALUE;
    XTrace("psxx11: client %lu connected\n", Client->Id);
    XWmLog("c%lu +", Client->Id);
    IssueRead(Client);
}

/**
 * @brief Creates a new pipe instance and waits asynchronously for the next connection.
 */
static
VOID
StartAccept(VOID)
{
    DWORD Error;

    ResetEvent(g_AcceptEvent);
    RtlZeroMemory(&g_AcceptOverlapped, sizeof(g_AcceptOverlapped));
    g_AcceptOverlapped.hEvent = g_AcceptEvent;
    g_AcceptPipe = CreateNamedPipeW(PSX_X11_PIPE_NAME,
                                    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                    PIPE_UNLIMITED_INSTANCES,
                                    0x10000,
                                    0x10000,
                                    0,
                                    NULL);
    if (g_AcceptPipe == INVALID_HANDLE_VALUE)
        return;

    if (ConnectNamedPipe(g_AcceptPipe, &g_AcceptOverlapped))
    {
        FinalizeClient();
        StartAccept();
        return;
    }

    Error = GetLastError();
    if (Error == ERROR_PIPE_CONNECTED)
    {
        FinalizeClient();
        StartAccept();
    }
    else if (Error != ERROR_IO_PENDING)
    {
        CloseHandle(g_AcceptPipe);
        g_AcceptPipe = INVALID_HANDLE_VALUE;
    }
}

static
VOID
OnAcceptDone(VOID)
{
    DWORD Transferred;

    if (GetOverlappedResult(g_AcceptPipe, &g_AcceptOverlapped, &Transferred, FALSE) ||
        GetLastError() == ERROR_PIPE_CONNECTED)
    {
        FinalizeClient();
    }
    else
    {
        CloseHandle(g_AcceptPipe);
        g_AcceptPipe = INVALID_HANDLE_VALUE;
    }
    StartAccept();
}

/* Win32 window */

static
VOID
BlitScreen(VOID)
{
    HDC WindowDc;

    if (!g_Hwnd)
        return;

    WindowDc = GetDC(g_Hwnd);
    if (WindowDc)
    {
        BitBlt(WindowDc, 0, 0, SCREEN_W, SCREEN_H, g_ScreenDc, 0, 0, SRCCOPY);
        ReleaseDC(g_Hwnd, WindowDc);
    }
}

static
VOID
CreateScreen(
    _In_ HWND Hwnd)
{
    HDC WindowDc = GetDC(Hwnd);
    BITMAPINFO Bmi;
    RECT Rect = { 0, 0, SCREEN_W, SCREEN_H };
    TEXTMETRICA Metrics;

    RtlZeroMemory(&Bmi, sizeof(Bmi));
    Bmi.bmiHeader.biSize = sizeof(Bmi.bmiHeader);
    Bmi.bmiHeader.biWidth = SCREEN_W;
    Bmi.bmiHeader.biHeight = -SCREEN_H;
    Bmi.bmiHeader.biPlanes = 1;
    Bmi.bmiHeader.biBitCount = 32;
    Bmi.bmiHeader.biCompression = BI_RGB;
    g_ScreenDc = CreateCompatibleDC(WindowDc);
    g_ScreenDib = CreateDIBSection(WindowDc, &Bmi, DIB_RGB_COLORS, &g_ScreenBits, NULL, 0);
    SelectObject(g_ScreenDc, g_ScreenDib);

    /* Measure the fixed font so QueryFont reports what GDI draws */
    SelectObject(g_ScreenDc, GetStockObject(ANSI_FIXED_FONT));
    if (GetTextMetricsA(g_ScreenDc, &Metrics))
    {
        g_FontWidth = Metrics.tmAveCharWidth;
        g_FontAscent = Metrics.tmAscent;
        g_FontDescent = Metrics.tmDescent;
        g_FontHeight = Metrics.tmHeight;
    }

    /* Root background */
    FillRect(g_ScreenDc, &Rect, (HBRUSH)GetStockObject(GRAY_BRUSH));
    ReleaseDC(Hwnd, WindowDc);
}

static
BYTE
MessageToButton(
    _In_ UINT Message)
{
    if (Message == WM_LBUTTONDOWN || Message == WM_LBUTTONUP)
        return 1;
    if (Message == WM_MBUTTONDOWN || Message == WM_MBUTTONUP)
        return 2;
    return 3;
}

static
LRESULT
CALLBACK
ServerWndProc(
    _In_ HWND Hwnd,
    _In_ UINT Message,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    /* Input is handled on the same thread as request dispatch, so no locking is needed */
    switch (Message)
    {
        case WM_CREATE:
            CreateScreen(Hwnd);
            return 0;

        case WM_PAINT:
        {
            PAINTSTRUCT Paint;
            HDC PaintDc = BeginPaint(Hwnd, &Paint);

            BitBlt(PaintDc, 0, 0, SCREEN_W, SCREEN_H, g_ScreenDc, 0, 0, SRCCOPY);
            EndPaint(Hwnd, &Paint);
            return 0;
        }

        case WM_MOUSEMOVE:
            g_PointerX = (SHORT)LOWORD(lParam);
            g_PointerY = (SHORT)HIWORD(lParam);
            UpdateCrossing(g_PointerX, g_PointerY);
            RoutePointer(MotionNotify, 0, PointerMotionMask, g_PointerX, g_PointerY, FALSE, FALSE);
            return 0;

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            g_PointerX = (SHORT)LOWORD(lParam);
            g_PointerY = (SHORT)HIWORD(lParam);
            SetCapture(Hwnd);
            RoutePointer(ButtonPress,
                         MessageToButton(Message),
                         ButtonPressMask,
                         g_PointerX,
                         g_PointerY,
                         TRUE,
                         FALSE);
            return 0;

        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
            g_PointerX = (SHORT)LOWORD(lParam);
            g_PointerY = (SHORT)HIWORD(lParam);
            ReleaseCapture();
            RoutePointer(ButtonRelease,
                         MessageToButton(Message),
                         ButtonReleaseMask,
                         g_PointerX,
                         g_PointerY,
                         FALSE,
                         TRUE);
            return 0;

        case WM_KEYDOWN:
            /* A keyboard grab takes all keys; otherwise focus follows the pointer */
            if (g_KeyboardGrab)
                DeliverToClient(g_KeyboardGrab, g_KeyboardGrabWindow, KeyPress, (BYTE)wParam, g_PointerX, g_PointerY);
            else
                DeliverInput(KeyPress, (BYTE)wParam, KeyPressMask, g_PointerX, g_PointerY);
            return 0;

        case WM_KEYUP:
            if (g_KeyboardGrab)
                DeliverToClient(g_KeyboardGrab, g_KeyboardGrabWindow, KeyRelease, (BYTE)wParam, g_PointerX, g_PointerY);
            else
                DeliverInput(KeyRelease, (BYTE)wParam, KeyReleaseMask, g_PointerX, g_PointerY);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(Hwnd, Message, wParam, lParam);
}

INT
WINAPI
wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ PWSTR lpCmdLine,
    _In_ INT nShowCmd)
{
    WNDCLASSEXW WndClass;
    MSG Msg;
    RECT Rect = { 0, 0, SCREEN_W, SCREEN_H };

    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nShowCmd);

    RtlZeroMemory(&WndClass, sizeof(WndClass));
    WndClass.cbSize = sizeof(WndClass);
    WndClass.lpfnWndProc = ServerWndProc;
    WndClass.hInstance = hInstance;
    WndClass.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    WndClass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    WndClass.lpszClassName = L"PsxX11Server";
    RegisterClassExW(&WndClass);

    AdjustWindowRect(&Rect, WS_OVERLAPPEDWINDOW, FALSE);
    g_Hwnd = CreateWindowExW(0,
                             L"PsxX11Server",
                             L"ReactOS X Server :0",
                             WS_OVERLAPPEDWINDOW,
                             40,
                             40,
                             Rect.right - Rect.left,
                             Rect.bottom - Rect.top,
                             NULL,
                             NULL,
                             hInstance,
                             NULL);
    if (!g_Hwnd)
        return 1;

    /* Do not take focus on startup; keystrokes stay with the console that launched the client */
    ShowWindow(g_Hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(g_Hwnd);
    XTrace("psxx11: server up, hwnd=%p\n", (PVOID)g_Hwnd);

    /* The root window covers the screen */
    g_Root = AllocateWindow(ID_ROOT, NULL, 0, 0, SCREEN_W, SCREEN_H);
    if (g_Root)
    {
        g_Root->Mapped = TRUE;
        g_Root->HasBackground = TRUE;
        g_Root->BackgroundPixel = 0x00808080;
    }

    /* One thread services pipe I/O and Win32 input together; no locks are needed */
    g_AcceptEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    StartAccept();

    for (;;)
    {
        HANDLE Handles[MAXIMUM_WAIT_OBJECTS];
        DWORD HandleCount = 0;
        PX_CLIENT Client;
        PX_CLIENT NextClient;

        Handles[HandleCount++] = g_AcceptEvent;
        for (Client = g_Clients; Client && HandleCount + 2 <= MAXIMUM_WAIT_OBJECTS; Client = Client->Next)
        {
            Handles[HandleCount++] = Client->ReadEvent;
            if (Client->WritePending)
                Handles[HandleCount++] = Client->WriteEvent;
        }

        /*
         * Wait for anything to become ready, or 33ms for the frame blit. The returned index
         * only names the lowest signaled handle, so every handle is polled afterwards to keep
         * a busy client from starving the others and the message queue.
         */
        MsgWaitForMultipleObjectsEx(HandleCount, Handles, 33, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        if (WaitForSingleObject(g_AcceptEvent, 0) == WAIT_OBJECT_0)
            OnAcceptDone();

        for (Client = g_Clients; Client; Client = NextClient)
        {
            /* Dead clients are freed later by ReapDeadClients */
            NextClient = Client->Next;
            if (WaitForSingleObject(Client->ReadEvent, 0) == WAIT_OBJECT_0)
                OnReadDone(Client);
            if (Client->Alive && Client->WritePending && WaitForSingleObject(Client->WriteEvent, 0) == WAIT_OBJECT_0)
                OnWriteDone(Client);
        }

        while (PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE))
        {
            if (Msg.message == WM_QUIT)
                return 0;
            TranslateMessage(&Msg);
            DispatchMessageW(&Msg);
        }

        ReapDeadClients();
        if (InterlockedExchange(&g_Dirty, 0))
            BlitScreen();
    }
}
