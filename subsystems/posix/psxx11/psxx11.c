/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     psxx11.exe -- the companion X Window System server. A compact but
 *              proper multi-client, multi-window X server: a Win32 GUI process that
 *              owns the screen (a GDI DIB) and speaks the X11 wire protocol to POSIX
 *              X clients whose bytes psxss relays over a named pipe (psxss/xconn.c).
 *
 *              Design (single-threaded, like a real X server -- no locks):
 *                - One thread runs everything. MsgWaitForMultipleObjects waits on the
 *                  overlapped read/write completions of every client pipe *and* the
 *                  Win32 input queue together. Requests are parsed from a per-client
 *                  input buffer; replies/events go into a per-client output buffer that
 *                  is flushed with overlapped writes, so a slow client never blocks the
 *                  server. Because there is exactly one thread, the window tree and
 *                  resource tables need no locking.
 *                - A window tree (root + children) with real geometry; drawing to a
 *                  window targets the screen DIB, translated to the window's absolute
 *                  position and clipped to its visible rect. Pixmaps are offscreen DIBs.
 *                - Substructure redirect + reparenting so a real window manager (9wm)
 *                  can frame and manage other clients' windows.
 *                - The DIB is blitted to the Win32 window on a ~30fps wait timeout.
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <windows.h>
#include <stdarg.h>

typedef unsigned long XID;          // X resource id (window/pixmap/gc/...)

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

/* ---- tracing ---- */
#define PSXX11_TRACE 1
#if PSXX11_TRACE
static void XDBG(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(buf, fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
}
#else
static void XDBG(const char *fmt, ...) { (void)fmt; }
#endif

/* ---- X protocol opcodes ---- */
enum {
    X_CreateWindow = 1, X_ChangeWindowAttributes = 2, X_GetWindowAttributes = 3,
    X_DestroyWindow = 4, X_ReparentWindow = 7, X_MapWindow = 8, X_MapSubwindows = 9,
    X_UnmapWindow = 10, X_UnmapSubwindows = 11,
    X_ConfigureWindow = 12, X_GetGeometry = 14, X_QueryTree = 15, X_InternAtom = 16,
    X_ChangeProperty = 18, X_DeleteProperty = 19, X_GetProperty = 20,
    X_GrabPointer = 26, X_UngrabPointer = 27, X_GrabButton = 28, X_UngrabButton = 29,
    X_GrabKey = 33, X_QueryPointer = 38,
    X_TranslateCoordinates = 40, X_SetInputFocus = 42, X_GetInputFocus = 43,
    X_OpenFont = 45, X_CloseFont = 46, X_QueryFont = 47, X_ListFonts = 49,
    X_CreatePixmap = 53,
    X_FreePixmap = 54, X_CreateGC = 55, X_ChangeGC = 56, X_CopyGC = 57,
    X_FreeGC = 60, X_ClearArea = 61,
    X_CopyArea = 62, X_PolyPoint = 64, X_PolyLine = 65, X_PolySegment = 66,
    X_PolyRectangle = 67, X_PolyArc = 68, X_FillPoly = 69, X_PolyFillRectangle = 70,
    X_PolyFillArc = 71, X_PutImage = 72, X_PolyText8 = 74, X_ImageText8 = 76,
    X_CreateColormap = 78,
    X_AllocColor = 84, X_AllocNamedColor = 85, X_LookupColor = 92,
    X_QueryExtension = 98
};

/*
 * Color-name database for AllocNamedColor/LookupColor (the rgb.txt classics
 * clients actually ask for; unknown names fall back to white). Case-blind,
 * spaces ignored ("navy blue" == "NavyBlue").
 */
static BOOL LookupColorName(const char *name, int n, WORD *r, WORD *g, WORD *b)
{
    static const struct { const char *name; BYTE r, g, b; } tab[] = {
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
        { "lightblue",   173, 216, 230 }, { "lightyellow",255, 255, 224 },
        { "wheat",       245, 222, 179 }, { "tan",        210, 180, 140 },
        { "maroon",      176, 48,  96  }, { "salmon",     250, 128, 114 },
        { "khaki",       240, 230, 140 }, { "plum",       221, 160, 221 },
        { "orchid",      218, 112, 214 }, { "coral",      255, 127, 80  },
        { "aquamarine",  127, 255, 212 }, { "chartreuse", 127, 255, 0   },
        { "firebrick",   178, 34,  34  }, { "goldenrod",  218, 165, 32  },
        { "seagreen",    46,  139, 87  }, { "slateblue",  106, 90,  205 },
        { "slategray",   112, 128, 144 }, { "ivory",      255, 255, 240 },
        { "snow",        255, 250, 250 }, { "beige",      245, 245, 220 },
        /* CDE/Motif + common rgb.txt colors clients actually request */
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
        { "gray10", 26,26,26 }, { "gray20", 51,51,51 }, { "gray30", 77,77,77 },
        { "gray40", 102,102,102 }, { "gray50", 127,127,127 }, { "gray60", 153,153,153 },
        { "gray70", 179,179,179 }, { "gray80", 204,204,204 }, { "gray90", 229,229,229 },
        { "grey10", 26,26,26 }, { "grey20", 51,51,51 }, { "grey30", 77,77,77 },
        { "grey40", 102,102,102 }, { "grey50", 127,127,127 }, { "grey60", 153,153,153 },
        { "grey70", 179,179,179 }, { "grey80", 204,204,204 }, { "grey90", 229,229,229 },
    };
    char clean[32];
    int i, k, m = 0;

    for (i = 0; i < n && m < (int)sizeof(clean) - 1; i++) {
        char ch = name[i];
        if (ch == ' ' || ch == '\t') continue;
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        clean[m++] = ch;
    }
    clean[m] = '\0';

    for (k = 0; k < (int)(sizeof(tab) / sizeof(tab[0])); k++) {
        if (lstrcmpA(clean, tab[k].name) == 0) {
            *r = (WORD)(tab[k].r << 8 | tab[k].r);
            *g = (WORD)(tab[k].g << 8 | tab[k].g);
            *b = (WORD)(tab[k].b << 8 | tab[k].b);
            return TRUE;
        }
    }
    *r = *g = *b = 0xFFFF;                      /* unknown -> white */
    return FALSE;
}

/* ---- X event codes ---- */
enum {
    KeyPress = 2, KeyRelease = 3, ButtonPress = 4, ButtonRelease = 5,
    MotionNotify = 6, EnterNotify = 7, LeaveNotify = 8, Expose = 12,
    CreateNotify = 16, DestroyNotify = 17, UnmapNotify = 18, MapNotify = 19,
    MapRequest = 20, ReparentNotify = 21, ConfigureNotify = 22,
    ConfigureRequest = 23, PropertyNotify = 28
};

/* ---- event-mask bits ---- */
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

/* ---- CreateWindow/ChangeWindowAttributes value bits ---- */
#define CWBackPixmap       0x0001
#define CWBackPixel        0x0002
#define CWBorderPixmap     0x0004
#define CWBorderPixel      0x0008
#define CWOverrideRedirect 0x0200
#define CWEventMask        0x0800

/* ---- GC component bits ---- */
#define GCFunction     0x0001
#define GCForeground   0x0004
#define GCBackground   0x0008
#define GCLineWidth    0x0010

/* ---- GC raster functions (subset) ---- */
#define GXclear   0x0
#define GXcopy    0x3
#define GXxor     0x6
#define GXinvert  0xa

/* ---- ConfigureWindow value bits ---- */
#define CWConfX 0x01
#define CWConfY 0x02
#define CWConfW 0x04
#define CWConfH 0x08
#define CWConfBorder 0x10
#define CWConfSibling 0x20
#define CWConfStack 0x40

#pragma pack(push, 1)
typedef struct { BYTE opcode; BYTE data; WORD length; } X_REQ_HEAD;
typedef struct { SHORT x, y; WORD width, height; } X_RECT;
typedef struct { SHORT x, y; } X_POINT;
typedef struct { SHORT x1, y1, x2, y2; } X_SEGMENT;
typedef struct { SHORT x, y; WORD width, height; SHORT angle1, angle2; } X_ARC;
#pragma pack(pop)

/* ---- objects ---- */
#define CS_HANDSHAKE 0
#define CS_RUNNING   1

typedef struct _XClient {
    struct _XClient *next;
    HANDLE   pipe;
    ULONG    id;
    BOOL     alive;
    int      state;                 // CS_HANDSHAKE / CS_RUNNING
    WORD     sequence;
    DWORD    idBase, idMask;
    // overlapped read
    OVERLAPPED rov;
    HANDLE   rev;                   // read-completion event (in the wait set)
    BYTE     rtmp[8192];            // read chunk
    BYTE    *inBuf; DWORD inLen, inCap;    // accumulated inbound bytes
    // overlapped write
    OVERLAPPED wov;
    HANDLE   wev;                   // write-completion event (in the wait set)
    BOOL     writePending;
    DWORD    writeInFlight;         // bytes of the current in-flight write
    BYTE    *outBuf; DWORD outLen, outCap; // pending outbound bytes
} XClient;

#define MAX_SELECT 8
typedef struct _XWin {
    struct _XWin *hnext;            // flat lookup list
    XID      id;
    struct _XWin *parent, *firstChild, *lastChild, *prevSib, *nextSib;
    int      x, y, w, h;            // x,y relative to parent origin
    int      borderWidth;
    DWORD    bgPixel, borderPixel;
    BOOL     hasBg;
    BOOL     mapped, overrideRedirect;
    struct { XClient *client; DWORD mask; } sel[MAX_SELECT];
    int      numSel;
    XClient *owner;
} XWin;

typedef struct _XGCObj {
    struct _XGCObj *next;
    XID   id;
    DWORD fg, bg, lineWidth, function;
} XGCObj;

typedef struct _XPixmap {
    struct _XPixmap *next;
    XID    id;
    int    w, h;
    HDC    dc;
    HBITMAP bmp;
    void  *bits;
} XPixmap;

/* ---- globals ---- */
static HWND      g_Hwnd;
static HDC       g_ScreenDc;
static HBITMAP   g_ScreenDib;
static void     *g_ScreenBits;
static volatile LONG g_Dirty;
static int       g_FontW = 8, g_FontAsc = 11, g_FontDesc = 2, g_FontH = 13;  // fixed font metrics

static XClient  *g_Clients;
static ULONG     g_ClientSeq;
static XWin     *g_Root;
static XWin     *g_AllWins;         // flat list (hnext)
static XGCObj   *g_GCs;
static XPixmap  *g_Pixmaps;
static int       g_PtrX, g_PtrY;
static XID       g_PtrWinId;        // window the pointer is currently inside
static DWORD     g_ServerTime = 1;  // monotonic server timestamp (ms-ish, for events)

// Pointer grabs. Passive button grabs auto-activate on a matching press; an explicit
// XGrabPointer stays until XUngrabPointer. While grabbed, all pointer events route to
// the grabbing client (this is how a WM does its menu / move / resize).
#define MAX_GRABS 16
static struct { XClient *c; XID win; int button; } g_BtnGrabs[MAX_GRABS];
static int       g_NumBtnGrabs;
static XClient  *g_PtrGrab;         // active pointer-grab client (NULL = none)
static XID       g_PtrGrabWin;
static BOOL      g_PtrGrabExplicit; // TRUE if from XGrabPointer (only Ungrab clears)

static COLORREF PixelToColor(DWORD p) { return RGB((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF); }

/* ================================================================================
 *  Overlapped pipe I/O + per-client output buffer (single-threaded, no locks)
 * ================================================================================ */
static BOOL SendSetupReply(XClient *c);
static void Dispatch(XClient *c, const X_REQ_HEAD *h, const BYTE *body, DWORD bodyLen);

static void EnsureCap(BYTE **buf, DWORD *cap, DWORD need)
{
    DWORD ncap;
    BYTE *nb;
    if (*cap >= need) return;
    ncap = *cap ? *cap : 8192;
    while (ncap < need) ncap *= 2;
    nb = *buf ? (BYTE *)HeapReAlloc(GetProcessHeap(), 0, *buf, ncap)
              : (BYTE *)HeapAlloc(GetProcessHeap(), 0, ncap);
    if (nb) { *buf = nb; *cap = ncap; }
}

// Kick an overlapped write of the pending output, if none is already in flight.
static void IssueWrite(XClient *c)
{
    if (!c->alive || c->writePending || c->outLen == 0) return;
    ResetEvent(c->wev);
    RtlZeroMemory(&c->wov, sizeof(c->wov));
    c->wov.hEvent = c->wev;
    c->writeInFlight = c->outLen;
    if (WriteFile(c->pipe, c->outBuf, c->outLen, NULL, &c->wov))
        c->writePending = TRUE;                  // completed sync; wev signals -> OnWriteDone
    else if (GetLastError() == ERROR_IO_PENDING)
        c->writePending = TRUE;
    else
        c->alive = FALSE;
}

// Queue bytes to a client (append to its output buffer; never blocks).
static void SendTo(XClient *c, const void *buf, DWORD len)
{
    if (!c || !c->alive) return;
    EnsureCap(&c->outBuf, &c->outCap, c->outLen + len);
    if (c->outCap < c->outLen + len) { c->alive = FALSE; return; }
    RtlCopyMemory(c->outBuf + c->outLen, buf, len);
    c->outLen += len;
    IssueWrite(c);
}

static void OnWriteDone(XClient *c)
{
    DWORD n = 0;
    c->writePending = FALSE;
    if (!GetOverlappedResult(c->pipe, &c->wov, &n, FALSE)) {
        if (GetLastError() != ERROR_IO_INCOMPLETE) c->alive = FALSE;
        return;
    }
    if (n > c->outLen) n = c->outLen;
    if (n < c->outLen) RtlMoveMemory(c->outBuf, c->outBuf + n, c->outLen - n);
    c->outLen -= n;
    IssueWrite(c);                               // flush anything appended meanwhile
    if (!c->writePending) ResetEvent(c->wev);    // nothing more: clear the manual event
}

// Consume complete requests (or the connection setup) from the input buffer.
static void ProcessInput(XClient *c)
{
    if (c->state == CS_HANDSHAKE) {
        DWORD nameLen, dataLen, need;
        if (c->inLen < 12) return;
        if (c->inBuf[0] != 'l') { c->alive = FALSE; return; }   // little-endian only
        nameLen = c->inBuf[6] | (c->inBuf[7] << 8);
        dataLen = c->inBuf[8] | (c->inBuf[9] << 8);
        need = 12 + ((nameLen + 3u) & ~3u) + ((dataLen + 3u) & ~3u);
        if (c->inLen < need) return;
        RtlMoveMemory(c->inBuf, c->inBuf + need, c->inLen - need);
        c->inLen -= need;
        SendSetupReply(c);
        c->state = CS_RUNNING;
        XDBG("psxx11: client %lu handshake ok\n", c->id);
    }
    while (c->state == CS_RUNNING && c->inLen >= 4) {
        DWORD reqLen = ((DWORD)(c->inBuf[2] | (c->inBuf[3] << 8))) * 4;
        X_REQ_HEAD head;
        if (reqLen < 4) { c->alive = FALSE; return; }
        if (c->inLen < reqLen) break;            // wait for the rest of this request
        head.opcode = c->inBuf[0]; head.data = c->inBuf[1];
        head.length = (WORD)(c->inBuf[2] | (c->inBuf[3] << 8));
        c->sequence++;
        Dispatch(c, &head, c->inBuf + 4, reqLen - 4);
        RtlMoveMemory(c->inBuf, c->inBuf + reqLen, c->inLen - reqLen);
        c->inLen -= reqLen;
    }
}

// Issue the next overlapped read for a client.
static void IssueRead(XClient *c)
{
    DWORD n;
    if (!c->alive) return;
    ResetEvent(c->rev);
    RtlZeroMemory(&c->rov, sizeof(c->rov));
    c->rov.hEvent = c->rev;
    if (!ReadFile(c->pipe, c->rtmp, sizeof(c->rtmp), &n, &c->rov)
        && GetLastError() != ERROR_IO_PENDING)
        c->alive = FALSE;
}

static void OnReadDone(XClient *c)
{
    DWORD n = 0;
    if (!GetOverlappedResult(c->pipe, &c->rov, &n, FALSE)) {
        if (GetLastError() != ERROR_IO_INCOMPLETE) c->alive = FALSE;
        return;
    }
    if (n == 0) { c->alive = FALSE; return; }    // EOF
    EnsureCap(&c->inBuf, &c->inCap, c->inLen + n);
    if (c->inCap < c->inLen + n) { c->alive = FALSE; return; }
    RtlCopyMemory(c->inBuf + c->inLen, c->rtmp, n);
    c->inLen += n;
    ProcessInput(c);
    if (c->alive) IssueRead(c);
}

static void ReplyHead(BYTE *rep, XClient *c, DWORD extraUnits)
{
    RtlZeroMemory(rep, 32);
    rep[0] = 1;
    rep[2] = (BYTE)(c->sequence & 0xFF);
    rep[3] = (BYTE)((c->sequence >> 8) & 0xFF);
    *(DWORD *)(rep + 4) = extraUnits;
}

/* ================================================================================
 *  Resource tables (single-threaded; no locking)
 * ================================================================================ */
static XWin *FindWin(XID id)
{
    XWin *w;
    for (w = g_AllWins; w; w = w->hnext)
        if (w->id == id) return w;
    return NULL;
}
static XGCObj *FindGC(XID id)
{
    XGCObj *g;
    for (g = g_GCs; g; g = g->next)
        if (g->id == id) return g;
    return NULL;
}
static XPixmap *FindPixmap(XID id)
{
    XPixmap *p;
    for (p = g_Pixmaps; p; p = p->next)
        if (p->id == id) return p;
    return NULL;
}

// Link child as the last (topmost) child of parent.
static void TreeLink(XWin *parent, XWin *child)
{
    child->parent = parent;
    child->nextSib = NULL;
    child->prevSib = parent->lastChild;
    if (parent->lastChild) parent->lastChild->nextSib = child;
    else parent->firstChild = child;
    parent->lastChild = child;
}
static void TreeUnlink(XWin *child)
{
    XWin *p = child->parent;
    if (!p) return;
    if (child->prevSib) child->prevSib->nextSib = child->nextSib;
    else p->firstChild = child->nextSib;
    if (child->nextSib) child->nextSib->prevSib = child->prevSib;
    else p->lastChild = child->prevSib;
    child->parent = child->prevSib = child->nextSib = NULL;
}

static XWin *NewWin(XID id, XWin *parent, int x, int y, int w, int h)
{
    XWin *win = (XWin *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(XWin));
    if (!win) return NULL;
    win->id = id;
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->bgPixel = WHITE_PIXEL;
    win->hnext = g_AllWins; g_AllWins = win;
    if (parent) TreeLink(parent, win);
    return win;
}

// Absolute (screen) position of a window's origin.
static void WinAbs(XWin *w, int *ax, int *ay)
{
    int x = 0, y = 0;
    while (w) { x += w->x; y += w->y; w = w->parent; }
    *ax = x; *ay = y;
}

// Visible rect: the window's absolute rect intersected with all ancestors' rects.
static void WinVisRect(XWin *w, RECT *r)
{
    int ax, ay;
    RECT win, anc;
    XWin *p;
    WinAbs(w, &ax, &ay);
    win.left = ax; win.top = ay; win.right = ax + w->w; win.bottom = ay + w->h;
    for (p = w->parent; p; p = p->parent) {
        int px, py;
        WinAbs(p, &px, &py);
        anc.left = px; anc.top = py; anc.right = px + p->w; anc.bottom = py + p->h;
        if (win.left < anc.left) win.left = anc.left;
        if (win.top < anc.top) win.top = anc.top;
        if (win.right > anc.right) win.right = anc.right;
        if (win.bottom > anc.bottom) win.bottom = anc.bottom;
    }
    if (win.right < win.left) win.right = win.left;
    if (win.bottom < win.top) win.bottom = win.top;
    *r = win;
}

static BOOL WinIsViewable(XWin *w)
{
    for (; w; w = w->parent) if (!w->mapped) return FALSE;
    return TRUE;
}

static BOOL IsAncestor(XWin *a, XWin *w)   // is a an ancestor of w (or a == w)?
{
    for (; w; w = w->parent) if (w == a) return TRUE;
    return FALSE;
}

// Is s stacked above w (i.e. s paints after w and can occlude it)? Children paint
// after (above) their parent; among siblings, later in the list is above.
static BOOL StackedAbove(XWin *s, XWin *w)
{
    XWin *a, *b, *x;
    int da = 0, db = 0;
    if (s == w) return FALSE;
    if (IsAncestor(w, s)) return TRUE;      // s is a descendant of w -> above
    if (IsAncestor(s, w)) return FALSE;     // s is an ancestor of w -> below
    for (x = s; x; x = x->parent) da++;
    for (x = w; x; x = x->parent) db++;
    a = s; b = w;
    while (da > db) { a = a->parent; da--; }
    while (db > da) { b = b->parent; db--; }
    while (a->parent != b->parent) { a = a->parent; b = b->parent; }
    for (x = b->nextSib; x; x = x->nextSib) if (x == a) return TRUE;   // a after b
    return FALSE;
}

// The actually-visible region of a window: its rect clipped to ancestors, minus every
// mapped window stacked above it that overlaps. Caller owns the returned HRGN.
static HRGN WinClipRegion(XWin *w)
{
    RECT vis, sr;
    HRGN rgn, sub;
    XWin *s;
    int ax, ay;
    WinVisRect(w, &vis);
    rgn = CreateRectRgnIndirect(&vis);
    for (s = g_AllWins; s; s = s->hnext) {
        if (s == w || !WinIsViewable(s)) continue;
        if (IsAncestor(s, w)) continue;
        if (!StackedAbove(s, w)) continue;
        WinAbs(s, &ax, &ay);
        sr.left = ax - s->borderWidth; sr.top = ay - s->borderWidth;
        sr.right = ax + s->w + s->borderWidth; sr.bottom = ay + s->h + s->borderWidth;
        sub = CreateRectRgnIndirect(&sr);
        CombineRgn(rgn, rgn, sub, RGN_DIFF);
        DeleteObject(sub);
    }
    return rgn;
}

// Raise a window to the top of its siblings (topmost = last child).
static void RaiseWin(XWin *w)
{
    XWin *p = w->parent;
    if (!p || p->lastChild == w) return;
    TreeUnlink(w);
    TreeLink(p, w);
}

/* ================================================================================
 *  Event delivery
 * ================================================================================ */
// Send a 32-byte event to every client that selected `mask` on `w`.
static void DeliverEvent(XWin *w, DWORD mask, BYTE *ev)
{
    int i, sent = 0;
    for (i = 0; i < w->numSel; i++) {
        if (w->sel[i].mask & mask) {
            XClient *c = w->sel[i].client;
            ev[2] = (BYTE)(c->sequence & 0xFF);
            ev[3] = (BYTE)((c->sequence >> 8) & 0xFF);
            SendTo(c, ev, 32);
            sent++;
        }
    }
    XDBG("psxx11: event %u win=%lx mask=%lx -> %d\n",                 /* TEMP motif */
         ev[0], (ULONG)w->id, mask, sent);
}
// The single client that selected SubstructureRedirect on w (the WM), if any, other
// than `except`.
static XClient *RedirectClient(XWin *w, XClient *except)
{
    int i;
    for (i = 0; i < w->numSel; i++)
        if ((w->sel[i].mask & SubstructureRedirectMask) && w->sel[i].client != except)
            return w->sel[i].client;
    return NULL;
}

static void SendExpose(XWin *w, int x, int y, int width, int height)
{
    BYTE ev[32];
    RtlZeroMemory(ev, sizeof(ev));
    ev[0] = Expose;
    *(DWORD *)(ev + 4) = (DWORD)w->id;
    *(WORD *)(ev + 8) = (WORD)x;
    *(WORD *)(ev + 10) = (WORD)y;
    *(WORD *)(ev + 12) = (WORD)width;
    *(WORD *)(ev + 14) = (WORD)height;
    DeliverEvent(w, ExposureMask, ev);
}

// Substructure/structure notify helper: send `ev` to SubstructureNotify selectors on
// the parent and StructureNotify selectors on the window itself.
static void NotifyStructure(XWin *w, BYTE *ev)
{
    if (w->parent) DeliverEvent(w->parent, SubstructureNotifyMask, ev);
    DeliverEvent(w, StructureNotifyMask, ev);
}

static void SendMapNotify(XWin *w)
{
    BYTE ev[32];
    RtlZeroMemory(ev, sizeof(ev));
    ev[0] = MapNotify;
    *(DWORD *)(ev + 4) = w->parent ? (DWORD)w->parent->id : 0;   // event window
    *(DWORD *)(ev + 8) = (DWORD)w->id;                            // window
    ev[12] = (BYTE)w->overrideRedirect;
    NotifyStructure(w, ev);
}
static void SendUnmapNotify(XWin *w)
{
    BYTE ev[32];
    RtlZeroMemory(ev, sizeof(ev));
    ev[0] = UnmapNotify;
    *(DWORD *)(ev + 4) = w->parent ? (DWORD)w->parent->id : 0;
    *(DWORD *)(ev + 8) = (DWORD)w->id;
    NotifyStructure(w, ev);
}
static void SendConfigureNotify(XWin *w)
{
    BYTE ev[32];
    int ax, ay;
    RtlZeroMemory(ev, sizeof(ev));
    WinAbs(w, &ax, &ay);
    ev[0] = ConfigureNotify;
    *(DWORD *)(ev + 4) = w->parent ? (DWORD)w->parent->id : 0;
    *(DWORD *)(ev + 8) = (DWORD)w->id;
    *(DWORD *)(ev + 12) = 0;                     // above-sibling: None
    *(WORD *)(ev + 16) = (WORD)w->x;
    *(WORD *)(ev + 18) = (WORD)w->y;
    *(WORD *)(ev + 20) = (WORD)w->w;
    *(WORD *)(ev + 22) = (WORD)w->h;
    *(WORD *)(ev + 24) = (WORD)w->borderWidth;
    NotifyStructure(w, ev);
}
static void SendCreateNotify(XWin *w)
{
    BYTE ev[32];
    RtlZeroMemory(ev, sizeof(ev));
    ev[0] = CreateNotify;
    *(DWORD *)(ev + 4) = w->parent ? (DWORD)w->parent->id : 0;   // parent
    *(DWORD *)(ev + 8) = (DWORD)w->id;                            // window
    *(WORD *)(ev + 12) = (WORD)w->x;
    *(WORD *)(ev + 14) = (WORD)w->y;
    *(WORD *)(ev + 16) = (WORD)w->w;
    *(WORD *)(ev + 18) = (WORD)w->h;
    *(WORD *)(ev + 20) = (WORD)w->borderWidth;
    ev[22] = (BYTE)w->overrideRedirect;
    if (w->parent) DeliverEvent(w->parent, SubstructureNotifyMask, ev);
}
static void SendDestroyNotify(XWin *w)
{
    BYTE ev[32];
    RtlZeroMemory(ev, sizeof(ev));
    ev[0] = DestroyNotify;
    *(DWORD *)(ev + 4) = w->parent ? (DWORD)w->parent->id : 0;
    *(DWORD *)(ev + 8) = (DWORD)w->id;
    NotifyStructure(w, ev);
}
static void SendReparentNotify(XWin *w)
{
    BYTE ev[32];
    RtlZeroMemory(ev, sizeof(ev));
    ev[0] = ReparentNotify;
    *(DWORD *)(ev + 4) = (DWORD)w->id;               // event
    *(DWORD *)(ev + 8) = (DWORD)w->id;               // window
    *(DWORD *)(ev + 12) = w->parent ? (DWORD)w->parent->id : 0;
    *(WORD *)(ev + 16) = (WORD)w->x;
    *(WORD *)(ev + 18) = (WORD)w->y;
    ev[20] = (BYTE)w->overrideRedirect;
    NotifyStructure(w, ev);
}

/* ================================================================================
 *  Painting
 * ================================================================================ */
static void FillAbsRect(RECT *r, DWORD pixel)
{
    HBRUSH b;
    if (r->right <= r->left || r->bottom <= r->top) return;
    b = CreateSolidBrush(PixelToColor(pixel));
    FillRect(g_ScreenDc, r, b);
    DeleteObject(b);
}

// Paint a window's border + background into its visible rect (no client content).
static void PaintWinBg(XWin *w)
{
    RECT vis;
    if (!WinIsViewable(w)) return;
    WinVisRect(w, &vis);
    if (w->borderWidth > 0 && w->parent) {
        int ax, ay;
        RECT br;
        WinAbs(w, &ax, &ay);
        br.left = ax - w->borderWidth; br.top = ay - w->borderWidth;
        br.right = ax + w->w + w->borderWidth; br.bottom = ay + w->h + w->borderWidth;
        FillAbsRect(&br, w->borderPixel);
    }
    FillAbsRect(&vis, w->hasBg ? w->bgPixel : WHITE_PIXEL);
}

// Recursively paint a window subtree bg (parents before children = back-to-front),
// then request the client to redraw content via Expose.
static void PaintTree(XWin *w)
{
    XWin *c;
    if (!w->mapped) return;
    PaintWinBg(w);
    for (c = w->firstChild; c; c = c->nextSib)
        PaintTree(c);
    if (w != g_Root)
        SendExpose(w, 0, 0, w->w, w->h);
}

// Full-screen repaint after a structural change: repaint the whole tree + expose.
static void RepaintScreen(void)
{
    PaintTree(g_Root);
    InterlockedExchange(&g_Dirty, 1);
}

/* ================================================================================
 *  Drawing (resolve a drawable to a target DC + offset + clip)
 * ================================================================================ */
typedef struct { HDC dc; int ox, oy; HRGN clip; BOOL valid; } XTarget;

static void ResolveDrawable(XID id, XTarget *t)
{
    XWin *w;
    XPixmap *p;
    t->valid = FALSE; t->clip = NULL;
    if ((w = FindWin(id)) != NULL) {
        int ax, ay;
        if (!WinIsViewable(w)) return;         // drawing to unmapped window: drop
        WinAbs(w, &ax, &ay);
        t->clip = WinClipRegion(w);            // visible region (minus occluders)
        t->dc = g_ScreenDc; t->ox = ax; t->oy = ay; t->valid = TRUE;
    } else if ((p = FindPixmap(id)) != NULL) {
        t->dc = p->dc; t->ox = 0; t->oy = 0;
        t->clip = CreateRectRgn(0, 0, p->w, p->h);
        t->valid = TRUE;
    }
}

static HRGN BeginTarget(XTarget *t)
{
    SelectClipRgn(t->dc, t->clip);
    return t->clip;
}
static void EndTarget(XTarget *t, HRGN rgn)
{
    (void)rgn;
    SelectClipRgn(t->dc, NULL);
    DeleteObject(t->clip);
    t->clip = NULL;
    InterlockedExchange(&g_Dirty, 1);
}

static void DrawFillRects(XTarget *t, XGCObj *gc, const X_RECT *rects, DWORD n)
{
    BOOL xorMode = (gc && gc->function == GXxor);
    HBRUSH b = CreateSolidBrush(PixelToColor(gc ? gc->fg : 0));
    HRGN rgn = BeginTarget(t);
    HBRUSH ob = (HBRUSH)SelectObject(t->dc, b);
    DWORD i;
    for (i = 0; i < n; i++) {
        int x = t->ox + rects[i].x, y = t->oy + rects[i].y;
        if (xorMode)
            PatBlt(t->dc, x, y, rects[i].width, rects[i].height, PATINVERT);  // dest ^= fg
        else {
            RECT r; r.left = x; r.top = y; r.right = x + rects[i].width; r.bottom = y + rects[i].height;
            FillRect(t->dc, &r, b);
        }
    }
    SelectObject(t->dc, ob);
    EndTarget(t, rgn);
    DeleteObject(b);
}
static void DrawFrameRects(XTarget *t, XGCObj *gc, const X_RECT *rects, DWORD n)
{
    HPEN pen = CreatePen(PS_SOLID, (gc && gc->lineWidth) ? gc->lineWidth : 1, PixelToColor(gc ? gc->fg : 0));
    HRGN rgn = BeginTarget(t);
    HPEN op = (HPEN)SelectObject(t->dc, pen);
    HBRUSH ob = (HBRUSH)SelectObject(t->dc, GetStockObject(NULL_BRUSH));
    int rop = SetROP2(t->dc, (gc && gc->function == GXxor) ? R2_XORPEN : R2_COPYPEN);
    DWORD i;
    for (i = 0; i < n; i++)
        Rectangle(t->dc, t->ox + rects[i].x, t->oy + rects[i].y,
                  t->ox + rects[i].x + rects[i].width + 1, t->oy + rects[i].y + rects[i].height + 1);
    SetROP2(t->dc, rop);
    SelectObject(t->dc, ob); SelectObject(t->dc, op);
    EndTarget(t, rgn);
    DeleteObject(pen);
}
static void DrawSegments(XTarget *t, XGCObj *gc, const X_SEGMENT *segs, DWORD n)
{
    HPEN pen = CreatePen(PS_SOLID, (gc && gc->lineWidth) ? gc->lineWidth : 1, PixelToColor(gc ? gc->fg : 0));
    HRGN rgn = BeginTarget(t);
    HPEN op = (HPEN)SelectObject(t->dc, pen);
    int rop = SetROP2(t->dc, (gc && gc->function == GXxor) ? R2_XORPEN : R2_COPYPEN);
    DWORD i;
    for (i = 0; i < n; i++) {
        MoveToEx(t->dc, t->ox + segs[i].x1, t->oy + segs[i].y1, NULL);
        LineTo(t->dc, t->ox + segs[i].x2, t->oy + segs[i].y2);
    }
    SetROP2(t->dc, rop);
    SelectObject(t->dc, op);
    EndTarget(t, rgn);
    DeleteObject(pen);
}
static void DrawPolyline(XTarget *t, XGCObj *gc, const X_POINT *pts, DWORD n, BYTE coordMode)
{
    HPEN pen, op;
    HRGN rgn;
    DWORD i;
    int cx, cy;
    if (n == 0) return;
    pen = CreatePen(PS_SOLID, (gc && gc->lineWidth) ? gc->lineWidth : 1, PixelToColor(gc ? gc->fg : 0));
    rgn = BeginTarget(t);
    op = (HPEN)SelectObject(t->dc, pen);
    cx = t->ox + pts[0].x; cy = t->oy + pts[0].y;
    MoveToEx(t->dc, cx, cy, NULL);
    for (i = 1; i < n; i++) {
        if (coordMode) { cx += pts[i].x; cy += pts[i].y; }
        else { cx = t->ox + pts[i].x; cy = t->oy + pts[i].y; }
        LineTo(t->dc, cx, cy);
    }
    SelectObject(t->dc, op);
    EndTarget(t, rgn);
    DeleteObject(pen);
}
static void DrawFillPoly(XTarget *t, XGCObj *gc, const X_POINT *pts, DWORD n)
{
    POINT *wp;
    HBRUSH b, ob;
    HPEN op;
    HRGN rgn;
    DWORD i;
    if (n < 2) return;
    wp = (POINT *)HeapAlloc(GetProcessHeap(), 0, n * sizeof(POINT));
    if (!wp) return;
    for (i = 0; i < n; i++) { wp[i].x = t->ox + pts[i].x; wp[i].y = t->oy + pts[i].y; }
    b = CreateSolidBrush(PixelToColor(gc ? gc->fg : 0));
    rgn = BeginTarget(t);
    ob = (HBRUSH)SelectObject(t->dc, b);
    op = (HPEN)SelectObject(t->dc, GetStockObject(NULL_PEN));
    Polygon(t->dc, wp, (int)n);
    SelectObject(t->dc, op); SelectObject(t->dc, ob);
    EndTarget(t, rgn);
    DeleteObject(b);
    HeapFree(GetProcessHeap(), 0, wp);
}
static void DrawFillArcs(XTarget *t, XGCObj *gc, const X_ARC *arcs, DWORD n)
{
    HBRUSH b, ob;
    HPEN op;
    HRGN rgn;
    DWORD i;
    b = CreateSolidBrush(PixelToColor(gc ? gc->fg : 0));
    rgn = BeginTarget(t);
    ob = (HBRUSH)SelectObject(t->dc, b);
    op = (HPEN)SelectObject(t->dc, GetStockObject(NULL_PEN));
    for (i = 0; i < n; i++)
        Ellipse(t->dc, t->ox + arcs[i].x, t->oy + arcs[i].y,
                t->ox + arcs[i].x + arcs[i].width + 1, t->oy + arcs[i].y + arcs[i].height + 1);
    SelectObject(t->dc, op); SelectObject(t->dc, ob);
    EndTarget(t, rgn);
    DeleteObject(b);
}
// Draw text. opaque=TRUE for ImageText8 (fills the bg box), FALSE for PolyText8 (glyphs
// only). GXxor is rendered via a glyph mask XORed onto the destination (SRCINVERT).
static void DrawTextGeneric(XTarget *t, XGCObj *gc, SHORT x, SHORT y, const char *s, int len, BOOL opaque)
{
    int px = t->ox + x, py = t->oy + y - g_FontAsc;   // X positions by baseline
    HRGN rgn;
    if (len <= 0) return;
    if (gc && gc->function == GXxor) {
        SIZE sz;
        rgn = BeginTarget(t);
        if (GetTextExtentPoint32A(t->dc, s, len, &sz) && sz.cx > 0 && sz.cy > 0) {
            HDC mem = CreateCompatibleDC(t->dc);
            HBITMAP bmp = CreateCompatibleBitmap(t->dc, sz.cx, sz.cy);
            HBITMAP obmp = (HBITMAP)SelectObject(mem, bmp);
            SelectObject(mem, GetStockObject(ANSI_FIXED_FONT));
            PatBlt(mem, 0, 0, sz.cx, sz.cy, BLACKNESS);        // black field
            SetTextColor(mem, RGB(255, 255, 255));             // white glyphs
            SetBkMode(mem, TRANSPARENT);
            TextOutA(mem, 0, 0, s, len);
            BitBlt(t->dc, px, py, sz.cx, sz.cy, mem, 0, 0, SRCINVERT);  // dest ^= glyph mask
            SelectObject(mem, obmp);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndTarget(t, rgn);
    } else {
        rgn = BeginTarget(t);
        SetTextColor(t->dc, PixelToColor(gc ? gc->fg : 0));
        SetBkColor(t->dc, PixelToColor(gc ? gc->bg : WHITE_PIXEL));
        SetBkMode(t->dc, opaque ? OPAQUE : TRANSPARENT);
        TextOutA(t->dc, px, py, s, len);
        EndTarget(t, rgn);
    }
}
static void DrawText8(XTarget *t, XGCObj *gc, SHORT x, SHORT y, const char *s, int len)
{
    DrawTextGeneric(t, gc, x, y, s, len, TRUE);       // ImageText8: opaque
}
// PutImage (ZPixmap, depth 24/32): blit the client's 32bpp rows into the drawable.
static void DrawPutImage(XTarget *t, SHORT dstX, SHORT dstY, WORD w, WORD h,
                         BYTE depth, const BYTE *data, DWORD dataLen)
{
    BITMAPINFO bmi;
    HRGN rgn;
    if ((depth != 24 && depth != 32) || w == 0 || h == 0) return;
    if ((DWORD)w * h * 4 > dataLen) return;
    RtlZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w; bmi.bmiHeader.biHeight = -(LONG)h;
    bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
    rgn = BeginTarget(t);
    SetDIBitsToDevice(t->dc, t->ox + dstX, t->oy + dstY, w, h, 0, 0, 0, h, data, &bmi, DIB_RGB_COLORS);
    EndTarget(t, rgn);
}

static void DoClearArea(XWin *w, SHORT x, SHORT y, WORD width, WORD height)
{
    XTarget t;
    HRGN rgn;
    HBRUSH b;
    RECT r;
    if (width == 0) width = (WORD)w->w;
    if (height == 0) height = (WORD)w->h;
    // Resolve through the window's visible region so the fill is clipped to the window's
    // OWN area, NOT its child windows -- e.g. a WM's XClearWindow on a frame must not
    // paint over the reparented client sitting inside it.
    ResolveDrawable(w->id, &t);
    if (!t.valid) { if (t.clip) DeleteObject(t.clip); return; }
    b = CreateSolidBrush(PixelToColor(w->hasBg ? w->bgPixel : WHITE_PIXEL));
    r.left = t.ox + x; r.top = t.oy + y; r.right = r.left + width; r.bottom = r.top + height;
    rgn = BeginTarget(&t);
    FillRect(t.dc, &r, b);
    EndTarget(&t, rgn);
    DeleteObject(b);
}
static void DoCopyArea(XID srcId, XID dstId, XGCObj *gc, int sx, int sy, int w, int h, int dx, int dy)
{
    XTarget src, dst;
    (void)gc;
    ResolveDrawable(srcId, &src);
    ResolveDrawable(dstId, &dst);
    if (!src.valid || !dst.valid) {
        if (src.clip) DeleteObject(src.clip);
        if (dst.clip) DeleteObject(dst.clip);
        return;
    }
    SelectClipRgn(dst.dc, dst.clip);
    BitBlt(dst.dc, dst.ox + dx, dst.oy + dy, w, h, src.dc, src.ox + sx, src.oy + sy, SRCCOPY);
    SelectClipRgn(dst.dc, NULL);
    DeleteObject(src.clip);
    DeleteObject(dst.clip);
    InterlockedExchange(&g_Dirty, 1);
}

/* ================================================================================
 *  Input routing
 * ================================================================================ */
// Deepest mapped window containing screen point (px,py); returns root if none deeper.
static XWin *WindowAtPoint(XWin *w, int px, int py)
{
    XWin *c, *hit = NULL;
    int ax, ay;
    WinAbs(w, &ax, &ay);
    if (px < ax || py < ay || px >= ax + w->w || py >= ay + w->h) return NULL;
    for (c = w->lastChild; c; c = c->prevSib) {     // topmost first
        if (!c->mapped) continue;
        hit = WindowAtPoint(c, px, py);
        if (hit) return hit;
    }
    return w;
}

// Send an EnterNotify/LeaveNotify (code 7/8) to the client(s) selecting on `w`.
static void SendCrossing(XWin *w, BYTE code, DWORD mask, int px, int py)
{
    BYTE ev[32];
    int ax, ay;
    WinAbs(w, &ax, &ay);
    RtlZeroMemory(ev, sizeof(ev));
    ev[0] = code;
    *(DWORD *)(ev + 8) = ID_ROOT;               // root
    *(DWORD *)(ev + 12) = (DWORD)w->id;         // event window
    *(WORD *)(ev + 20) = (WORD)px;
    *(WORD *)(ev + 22) = (WORD)py;
    *(WORD *)(ev + 24) = (WORD)(px - ax);
    *(WORD *)(ev + 26) = (WORD)(py - ay);
    ev[31] = 1;                                 // same-screen
    DeliverEvent(w, mask, ev);
}

// Update the pointer window on motion; emit Leave/Enter when it changes.
static void UpdateCrossing(int px, int py)
{
    XWin *now = WindowAtPoint(g_Root, px, py);
    XWin *old;
    if (!now) now = g_Root;
    if (now->id == g_PtrWinId) return;
    old = FindWin(g_PtrWinId);
    if (old) SendCrossing(old, LeaveNotify, LeaveWindowMask, px, py);
    SendCrossing(now, EnterNotify, EnterWindowMask, px, py);
    g_PtrWinId = now->id;
}

// Deliver a device event (button/key/motion) to the window under the pointer,
// propagating up to an ancestor that selected the mask.
static void DeliverInput(BYTE code, BYTE detail, DWORD mask, int px, int py)
{
    XWin *w = WindowAtPoint(g_Root, px, py);
    XWin *e;
    if (!w) w = g_Root;
    for (e = w; e; e = e->parent) {
        int i;
        for (i = 0; i < e->numSel; i++) {
            if (e->sel[i].mask & mask) {
                XClient *c = e->sel[i].client;
                int ax, ay;
                BYTE ev[32];
                WinAbs(e, &ax, &ay);
                RtlZeroMemory(ev, sizeof(ev));
                ev[0] = code;
                ev[1] = detail;
                ev[2] = (BYTE)(c->sequence & 0xFF);
                ev[3] = (BYTE)((c->sequence >> 8) & 0xFF);
                *(DWORD *)(ev + 8) = ID_ROOT;                 // root
                *(DWORD *)(ev + 12) = (DWORD)e->id;           // event window
                *(WORD *)(ev + 20) = (WORD)px;                // root-x
                *(WORD *)(ev + 22) = (WORD)py;                // root-y
                *(WORD *)(ev + 24) = (WORD)(px - ax);         // event-x
                *(WORD *)(ev + 26) = (WORD)(py - ay);         // event-y
                ev[30] = 1;                                   // same-screen
                SendTo(c, ev, 32);
                return;                                       // delivered
            }
        }
    }
}

// Deliver a device event straight to a specific client with a given event window.
static void DeliverToClient(XClient *c, XID eventWin, BYTE code, BYTE detail, int px, int py)
{
    XWin *w = FindWin(eventWin);
    int ax = 0, ay = 0;
    BYTE ev[32];
    if (w) WinAbs(w, &ax, &ay);
    RtlZeroMemory(ev, sizeof(ev));
    ev[0] = code; ev[1] = detail;
    ev[2] = (BYTE)(c->sequence & 0xFF); ev[3] = (BYTE)((c->sequence >> 8) & 0xFF);
    *(DWORD *)(ev + 8) = ID_ROOT;
    *(DWORD *)(ev + 12) = eventWin;
    *(WORD *)(ev + 20) = (WORD)px; *(WORD *)(ev + 22) = (WORD)py;
    *(WORD *)(ev + 24) = (WORD)(px - ax); *(WORD *)(ev + 26) = (WORD)(py - ay);
    ev[30] = 1;
    SendTo(c, ev, 32);
}

// Route a pointer event honoring grabs: a passive button grab auto-activates on a
// matching press; while any grab is active the event goes to the grabbing client.
static void RoutePointer(BYTE code, BYTE detail, DWORD mask, int px, int py, BOOL isPress, BOOL isRelease)
{
    if (isPress && !g_PtrGrab) {
        // Walk from the pointer window UP to root; the DEEPEST window with a matching
        // passive grab wins (X grab specificity). So a click over a managed client hits
        // the frame's grab (9wm can raise/activate it), while a click on the bare root
        // hits the root menu grab.
        XWin *w;
        for (w = WindowAtPoint(g_Root, px, py); w && !g_PtrGrab; w = w->parent) {
            int i;
            for (i = 0; i < g_NumBtnGrabs; i++) {
                if (g_BtnGrabs[i].win == w->id &&
                    (g_BtnGrabs[i].button == 0 || g_BtnGrabs[i].button == detail)) {
                    g_PtrGrab = g_BtnGrabs[i].c;
                    g_PtrGrabWin = w->id;
                    g_PtrGrabExplicit = FALSE;
                    break;
                }
            }
        }
    }
    if (g_PtrGrab)
        DeliverToClient(g_PtrGrab, g_PtrGrabWin, code, detail, px, py);
    else
        DeliverInput(code, detail, mask, px, py);
    if (isRelease && g_PtrGrab && !g_PtrGrabExplicit)
        g_PtrGrab = NULL;
}

/* ================================================================================
 *  Connection setup (handshake)
 * ================================================================================ */
static BOOL SendSetupReply(XClient *c)
{
    static const char vendor[] = "ReactOS-PSX";
    BYTE buf[512];
    BYTE *p = buf;
    DWORD vlen = (DWORD)(sizeof(vendor) - 1);
    DWORD vpad = (4 - (vlen & 3)) & 3;
    DWORD adBytes;

#define PUT8(v)  (*p++ = (BYTE)(v))
#define PUT16(v) do { WORD _w=(WORD)(v); *p++=_w&0xFF; *p++=(_w>>8)&0xFF; } while(0)
#define PUT32(v) do { DWORD _d=(DWORD)(v); *p++=_d&0xFF; *p++=(_d>>8)&0xFF; *p++=(_d>>16)&0xFF; *p++=(_d>>24)&0xFF; } while(0)

    PUT8(1); PUT8(0); PUT16(11); PUT16(0); PUT16(0);          // prefix (ad length patched)
    PUT32(1);                                                 // release
    PUT32(c->idBase); PUT32(c->idMask);                       // resource id base/mask
    PUT32(0);                                                 // motion buffer
    PUT16(vlen); PUT16(65535);                                // vendor len, max req
    PUT8(1); PUT8(1);                                         // #screens, #formats
    PUT8(0); PUT8(0); PUT8(32); PUT8(32);                     // byte/bit order, unit, pad
    PUT8(8); PUT8(255); PUT32(0);                             // min/max keycode, unused
    RtlCopyMemory(p, vendor, vlen); p += vlen;
    while (vpad--) PUT8(0);
    // FORMAT
    PUT8(24); PUT8(32); PUT8(32); PUT8(0); PUT32(0);
    // SCREEN
    PUT32(ID_ROOT); PUT32(ID_COLORMAP);
    PUT32(WHITE_PIXEL); PUT32(BLACK_PIXEL);
    PUT32(0);                                                 // current-input-masks
    PUT16(SCREEN_W); PUT16(SCREEN_H);
    PUT16(211); PUT16(158);                                   // mm
    PUT16(1); PUT16(1);                                       // min/max maps
    PUT32(ID_VISUAL);
    PUT8(0); PUT8(0); PUT8(24); PUT8(1);                      // backing/save/rootdepth/#depths
    // DEPTH
    PUT8(24); PUT8(0); PUT16(1); PUT32(0);
    // VISUALTYPE
    PUT32(ID_VISUAL); PUT8(4); PUT8(8); PUT16(256);
    PUT32(0x00FF0000); PUT32(0x0000FF00); PUT32(0x000000FF); PUT32(0);

    adBytes = (DWORD)(p - buf) - 8;
    buf[6] = (BYTE)((adBytes / 4) & 0xFF);
    buf[7] = (BYTE)(((adBytes / 4) >> 8) & 0xFF);
#undef PUT8
#undef PUT16
#undef PUT32
    SendTo(c, buf, (DWORD)(p - buf));            // queue via the output buffer
    return TRUE;
}

/* ================================================================================
 *  Request handlers (single-threaded; no locks needed)
 * ================================================================================ */
static void AddSelector(XWin *w, XClient *c, DWORD mask)
{
    int i;
    for (i = 0; i < w->numSel; i++)
        if (w->sel[i].client == c) { w->sel[i].mask = mask; return; }
    if (w->numSel < MAX_SELECT) {
        w->sel[w->numSel].client = c;
        w->sel[w->numSel].mask = mask;
        w->numSel++;
    }
}

// Extract value from a CW/GC value-list given the mask bit.
static DWORD ValueFor(DWORD mask, DWORD bit, const DWORD *values, DWORD count)
{
    DWORD idx = 0, b;
    if (!(mask & bit)) return 0;
    for (b = 1; b < bit; b <<= 1) if (mask & b) idx++;
    return (idx < count) ? values[idx] : 0;
}

static void DoMapWindow(XClient *c, XWin *w)
{
    XClient *wm;
    if (w->mapped) return;
    wm = w->parent ? RedirectClient(w->parent, c) : NULL;
    if (wm && !w->overrideRedirect) {
        // Redirect: tell the WM (MapRequest) instead of mapping.
        BYTE ev[32];
        RtlZeroMemory(ev, sizeof(ev));
        ev[0] = MapRequest;
        ev[2] = (BYTE)(wm->sequence & 0xFF);
        ev[3] = (BYTE)((wm->sequence >> 8) & 0xFF);
        *(DWORD *)(ev + 4) = (DWORD)w->parent->id;   // event (parent)
        *(DWORD *)(ev + 8) = (DWORD)w->id;           // window
        SendTo(wm, ev, 32);
        return;
    }
    w->mapped = TRUE;
    SendMapNotify(w);
    RepaintScreen();
}

static void DoConfigureWindow(XClient *c, XWin *w, DWORD mask, const DWORD *values, DWORD count)
{
    XClient *wm = w->parent ? RedirectClient(w->parent, c) : NULL;
    if (wm && !w->overrideRedirect) {
        BYTE ev[32];
        RtlZeroMemory(ev, sizeof(ev));
        ev[0] = ConfigureRequest;
        ev[2] = (BYTE)(wm->sequence & 0xFF);
        ev[3] = (BYTE)((wm->sequence >> 8) & 0xFF);
        *(DWORD *)(ev + 4) = (DWORD)w->parent->id;   // parent
        *(DWORD *)(ev + 8) = (DWORD)w->id;           // window
        if (mask & CWConfX) *(WORD *)(ev + 16) = (WORD)ValueFor(mask, CWConfX, values, count);
        if (mask & CWConfY) *(WORD *)(ev + 18) = (WORD)ValueFor(mask, CWConfY, values, count);
        if (mask & CWConfW) *(WORD *)(ev + 20) = (WORD)ValueFor(mask, CWConfW, values, count);
        if (mask & CWConfH) *(WORD *)(ev + 22) = (WORD)ValueFor(mask, CWConfH, values, count);
        *(WORD *)(ev + 26) = (WORD)mask;             // value-mask
        SendTo(wm, ev, 32);
        return;
    }
    if (mask & CWConfX) w->x = (SHORT)ValueFor(mask, CWConfX, values, count);
    if (mask & CWConfY) w->y = (SHORT)ValueFor(mask, CWConfY, values, count);
    if (mask & CWConfW) w->w = (WORD)ValueFor(mask, CWConfW, values, count);
    if (mask & CWConfH) w->h = (WORD)ValueFor(mask, CWConfH, values, count);
    if (mask & CWConfBorder) w->borderWidth = (WORD)ValueFor(mask, CWConfBorder, values, count);
    if (mask & CWConfStack) {
        DWORD sm = ValueFor(mask, CWConfStack, values, count);
        if (sm == 0) RaiseWin(w);       // Above (raise to top -- click-to-raise)
        else if (sm == 1 && w->parent) { // Below (lower to bottom)
            XWin *p = w->parent;
            TreeUnlink(w);
            w->parent = p; w->prevSib = NULL; w->nextSib = p->firstChild;
            if (p->firstChild) p->firstChild->prevSib = w; else p->lastChild = w;
            p->firstChild = w;
        }
    }
    SendConfigureNotify(w);
    RepaintScreen();
}

// Dispatch one request. Returns TRUE to keep serving.
// The core X11 protocol's reply-bearing requests (fixed set, X11R1..R7).
// An unhandled member of this set MUST be answered (with an error) or the
// client blocks forever waiting for the reply.
static BOOL IsReplyOp(BYTE op)
{
    switch (op) {
    case 3:   /* GetWindowAttributes */  case 14:  /* GetGeometry */
    case 15:  /* QueryTree */            case 16:  /* InternAtom */
    case 17:  /* GetAtomName */          case 20:  /* GetProperty */
    case 21:  /* ListProperties */       case 23:  /* GetSelectionOwner */
    case 26:  /* GrabPointer */          case 31:  /* GrabKeyboard */
    case 38:  /* QueryPointer */         case 39:  /* GetMotionEvents */
    case 40:  /* TranslateCoords */      case 43:  /* GetInputFocus */
    case 44:  /* QueryKeymap */          case 47:  /* QueryFont */
    case 48:  /* QueryTextExtents */     case 49:  /* ListFonts */
    case 50:  /* ListFontsWithInfo */    case 52:  /* GetFontPath */
    case 73:  /* GetImage */             case 83:  /* ListInstalledCmaps */
    case 84:  /* AllocColor */           case 85:  /* AllocNamedColor */
    case 86:  /* AllocColorCells */      case 87:  /* AllocColorPlanes */
    case 91:  /* QueryColors */          case 92:  /* LookupColor */
    case 97:  /* QueryBestSize */        case 98:  /* QueryExtension */
    case 99:  /* ListExtensions */       case 101: /* GetKeyboardMapping */
    case 103: /* GetKeyboardControl */   case 106: /* GetPointerControl */
    case 108: /* GetScreenSaver */       case 110: /* ListHosts */
    case 116: /* SetPointerMapping */    case 117: /* GetPointerMapping */
    case 118: /* SetModifierMapping */   case 119: /* GetModifierMapping */
        return TRUE;
    default:
        return FALSE;
    }
}

/*
 * Keycode -> keysym for GetKeyboardMapping.  psxx11 delivers KeyPress with the
 * Windows VK code as the X keycode (see WM_KEYDOWN), so the map is keyed by VK.
 * US layout; shift==0 base, shift==1 shifted.  Returns NoSymbol (0) if unmapped.
 */
static DWORD KeycodeToKeysym(BYTE vk, int shift)
{
    if (vk >= 'A' && vk <= 'Z')                 /* letters: XK_A..Z / XK_a..z */
        return shift ? (DWORD)vk : (DWORD)(vk + 32);
    if (vk >= '0' && vk <= '9') {               /* digits + US shifted symbols */
        static const char sh[] = ")!@#$%^&*(";
        return shift ? (DWORD)sh[vk - '0'] : (DWORD)vk;
    }
    if (vk >= 0x70 && vk <= 0x7B)               /* F1..F12 -> XK_F1.. (0xFFBE) */
        return 0xFFBE + (vk - 0x70);
    switch (vk) {
    case 0x20: return 0x20;                      /* space */
    case 0x0D: return 0xFF0D;                    /* Return */
    case 0x08: return 0xFF08;                    /* BackSpace */
    case 0x09: return 0xFF09;                    /* Tab */
    case 0x1B: return 0xFF1B;                    /* Escape */
    case 0x2E: return 0xFFFF;                    /* Delete */
    case 0x2D: return 0xFF63;                    /* Insert */
    case 0x24: return 0xFF50;                    /* Home */
    case 0x23: return 0xFF57;                    /* End */
    case 0x21: return 0xFF55;                    /* Page_Up */
    case 0x22: return 0xFF56;                    /* Page_Down */
    case 0x25: return 0xFF51;                    /* Left */
    case 0x26: return 0xFF52;                    /* Up */
    case 0x27: return 0xFF53;                    /* Right */
    case 0x28: return 0xFF54;                    /* Down */
    case 0x10: return 0xFFE1;                    /* Shift_L */
    case 0x11: return 0xFFE3;                    /* Control_L */
    case 0x12: return 0xFFE9;                    /* Alt_L */
    case 0x14: return 0xFFE5;                    /* Caps_Lock */
    case 0xBA: return shift ? ':' : ';';         /* OEM_1 */
    case 0xBB: return shift ? '+' : '=';         /* OEM_PLUS */
    case 0xBC: return shift ? '<' : ',';         /* OEM_COMMA */
    case 0xBD: return shift ? '_' : '-';         /* OEM_MINUS */
    case 0xBE: return shift ? '>' : '.';         /* OEM_PERIOD */
    case 0xBF: return shift ? '?' : '/';         /* OEM_2 */
    case 0xC0: return shift ? '~' : '`';         /* OEM_3 */
    case 0xDB: return shift ? '{' : '[';         /* OEM_4 */
    case 0xDC: return shift ? '|' : '\\';        /* OEM_5 */
    case 0xDD: return shift ? '}' : ']';         /* OEM_6 */
    case 0xDE: return shift ? '"' : '\'';        /* OEM_7 */
    }
    return 0;                                    /* NoSymbol */
}

static void Dispatch(XClient *c, const X_REQ_HEAD *h, const BYTE *body, DWORD bodyLen)
{
    XDBG("psxx11: c%lu req op=%u len=%lu\n", c->id, h->opcode, bodyLen);  /* TEMP motif */
    switch (h->opcode) {
    case X_CreateWindow: {
        // depth(data) wid(4) parent(4) x(2)y(2)w(2)h(2)bw(2)class(2) visual(4) mask(4) values
        if (bodyLen >= 28) {
            XID wid = *(const DWORD *)(body + 0);
            XID pid = *(const DWORD *)(body + 4);
            XWin *parent = FindWin(pid);
            SHORT x = *(const SHORT *)(body + 8), y = *(const SHORT *)(body + 10);
            WORD ww = *(const WORD *)(body + 12), hh = *(const WORD *)(body + 14);
            WORD bw = *(const WORD *)(body + 16);
            DWORD mask = *(const DWORD *)(body + 24);
            const DWORD *vals = (const DWORD *)(body + 28);
            DWORD vcount = (bodyLen - 28) / 4;
            XWin *w;
            if (!parent) parent = g_Root;
            w = NewWin(wid, parent, x, y, ww, hh);
            if (w) {
                w->owner = c; w->borderWidth = bw;
                if (mask & CWBackPixel) { w->bgPixel = ValueFor(mask, CWBackPixel, vals, vcount); w->hasBg = TRUE; }
                if (mask & CWBorderPixel) w->borderPixel = ValueFor(mask, CWBorderPixel, vals, vcount);
                if (mask & CWOverrideRedirect) w->overrideRedirect = ValueFor(mask, CWOverrideRedirect, vals, vcount) != 0;
                if (mask & CWEventMask) AddSelector(w, c, ValueFor(mask, CWEventMask, vals, vcount));
                SendCreateNotify(w);
            }
        }
        break;
    }
    case X_ChangeWindowAttributes: {
        if (bodyLen >= 8) {
            XWin *w = FindWin(*(const DWORD *)(body + 0));
            DWORD mask = *(const DWORD *)(body + 4);
            const DWORD *vals = (const DWORD *)(body + 8);
            DWORD vcount = (bodyLen - 8) / 4;
            if (w) {
                if (mask & CWBackPixel) { w->bgPixel = ValueFor(mask, CWBackPixel, vals, vcount); w->hasBg = TRUE; }
                if (mask & CWBorderPixel) w->borderPixel = ValueFor(mask, CWBorderPixel, vals, vcount);
                if (mask & CWEventMask) AddSelector(w, c, ValueFor(mask, CWEventMask, vals, vcount));
            }
        }
        break;
    }
    case X_GetWindowAttributes: {
        XWin *w = FindWin(*(const DWORD *)body);
        BYTE rep[44];
        ReplyHead(rep, c, 3);
        rep[1] = 0;
        *(DWORD *)(rep + 8) = ID_VISUAL;
        *(WORD *)(rep + 12) = 1;                         // InputOutput
        rep[26] = (BYTE)(w && w->mapped ? 2 : 0);        // map-state
        rep[27] = (BYTE)(w ? w->overrideRedirect : 0);
        *(DWORD *)(rep + 28) = ID_COLORMAP;
        if (w) *(DWORD *)(rep + 36) = w->numSel ? w->sel[0].mask : 0;   // your-event-mask
        SendTo(c, rep, 44);
        break;
    }
    case X_DestroyWindow: {
        XWin *w = FindWin(*(const DWORD *)body);
        if (w && w != g_Root) {
            XWin **pp;
            SendDestroyNotify(w);
            if (w->mapped) { w->mapped = FALSE; }
            TreeUnlink(w);
            for (pp = &g_AllWins; *pp; pp = &(*pp)->hnext)
                if (*pp == w) { *pp = w->hnext; break; }
            HeapFree(GetProcessHeap(), 0, w);
            RepaintScreen();
        }
        break;
    }
    case X_ReparentWindow: {
        if (bodyLen >= 12) {
            XWin *w = FindWin(*(const DWORD *)(body + 0));
            XWin *np = FindWin(*(const DWORD *)(body + 4));
            SHORT x = *(const SHORT *)(body + 8), y = *(const SHORT *)(body + 10);
            if (w && np && w != g_Root) {
                BOOL wasMapped = w->mapped;
                if (wasMapped) { w->mapped = FALSE; }
                TreeUnlink(w);
                TreeLink(np, w);
                w->x = x; w->y = y;
                SendReparentNotify(w);
                if (wasMapped) { w->mapped = TRUE; SendMapNotify(w); }
                RepaintScreen();
            }
        }
        break;
    }
    case X_MapWindow: {
        XWin *w = FindWin(*(const DWORD *)body);
        if (w) DoMapWindow(c, w);
        break;
    }
    case X_MapSubwindows: {
        // Map every unmapped child of the window (Xt realizes children this
        // way: XMapSubwindows(shell) then XMapWindow(shell)).  DoMapWindow
        // honors substructure redirect per child and repaints/exposes.
        XWin *w = FindWin(*(const DWORD *)body);
        if (w) {
            XWin *k;
            for (k = w->firstChild; k; k = k->nextSib)
                DoMapWindow(c, k);
        }
        break;
    }
    case X_UnmapWindow: {
        XWin *w = FindWin(*(const DWORD *)body);
        if (w && w->mapped) {
            w->mapped = FALSE;
            SendUnmapNotify(w);
            RepaintScreen();
        }
        break;
    }
    case X_UnmapSubwindows: {
        XWin *w = FindWin(*(const DWORD *)body);
        if (w) {
            XWin *k;
            for (k = w->firstChild; k; k = k->nextSib) {
                if (k->mapped) {
                    k->mapped = FALSE;
                    SendUnmapNotify(k);
                }
            }
            RepaintScreen();
        }
        break;
    }
    case X_ConfigureWindow: {
        if (bodyLen >= 8) {
            XWin *w = FindWin(*(const DWORD *)(body + 0));
            WORD mask = *(const WORD *)(body + 4);
            const DWORD *vals = (const DWORD *)(body + 8);
            DWORD vcount = (bodyLen - 8) / 4;
            if (w) DoConfigureWindow(c, w, mask, vals, vcount);
        }
        break;
    }
    case X_GetGeometry: {
        XWin *w = FindWin(*(const DWORD *)body);
        BYTE rep[32];
        ReplyHead(rep, c, 0);
        rep[1] = 24;                                    // depth
        *(DWORD *)(rep + 8) = ID_ROOT;                  // root
        if (w) {
            *(WORD *)(rep + 12) = (WORD)w->x;
            *(WORD *)(rep + 14) = (WORD)w->y;
            *(WORD *)(rep + 16) = (WORD)w->w;
            *(WORD *)(rep + 18) = (WORD)w->h;
            *(WORD *)(rep + 20) = (WORD)w->borderWidth;
        } else {
            *(WORD *)(rep + 16) = SCREEN_W;
            *(WORD *)(rep + 18) = SCREEN_H;
        }
        SendTo(c, rep, 32);
        break;
    }
    case X_QueryTree: {
        XWin *w = FindWin(*(const DWORD *)body);
        BYTE rep[512];
        WORD nchild = 0;
        DWORD off = 32;
        RtlZeroMemory(rep, sizeof(rep));
        rep[0] = 1;
        rep[2] = (BYTE)(c->sequence & 0xFF);
        rep[3] = (BYTE)((c->sequence >> 8) & 0xFF);
        *(DWORD *)(rep + 8) = ID_ROOT;                              // root
        *(DWORD *)(rep + 12) = (w && w->parent) ? (DWORD)w->parent->id : 0;
        if (w) {
            XWin *ch;
            for (ch = w->firstChild; ch && off + 4 <= sizeof(rep); ch = ch->nextSib) {
                *(DWORD *)(rep + off) = (DWORD)ch->id; off += 4; nchild++;
            }
        }
        *(WORD *)(rep + 16) = nchild;
        *(DWORD *)(rep + 4) = nchild;                               // reply length (units)
        SendTo(c, rep, 32 + nchild * 4);
        break;
    }
    case X_InternAtom: {
        BYTE rep[32];
        ReplyHead(rep, c, 0);
        *(DWORD *)(rep + 8) = 1 + (bodyLen & 0xFFFF);               // stable non-zero atom
        SendTo(c, rep, 32);
        break;
    }
    case X_ChangeProperty: {
        // window(4) property(4) type(4) format(1) ... We don't store properties, but we
        // MUST emit PropertyNotify: WMs (9wm's timestamp()) do a zero-length append and
        // block in XMaskEvent(PropertyChangeMask) to fetch the server time.
        if (bodyLen >= 8) {
            XWin *w = FindWin(*(const DWORD *)(body + 0));
            if (w) {
                BYTE ev[32];
                RtlZeroMemory(ev, sizeof(ev));
                ev[0] = PropertyNotify;
                *(DWORD *)(ev + 4) = (DWORD)w->id;                  // window
                *(DWORD *)(ev + 8) = *(const DWORD *)(body + 4);    // atom (property)
                *(DWORD *)(ev + 12) = g_ServerTime++;               // time (must be non-zero)
                ev[16] = 0;                                         // state: NewValue
                DeliverEvent(w, PropertyChangeMask, ev);
            }
        }
        break;
    }
    case X_GetProperty: {
        BYTE rep[32];
        ReplyHead(rep, c, 0);
        SendTo(c, rep, 32);                                         // empty property
        break;
    }
    case X_GetInputFocus: {
        BYTE rep[32];
        ReplyHead(rep, c, 0);
        rep[1] = 0;                                                 // revert-to
        *(DWORD *)(rep + 8) = ID_ROOT;                              // focus
        SendTo(c, rep, 32);
        break;
    }
    case X_QueryPointer: {
        BYTE rep[32];
        ReplyHead(rep, c, 0);
        rep[1] = 1;                                                 // same-screen
        *(DWORD *)(rep + 8) = ID_ROOT;
        *(WORD *)(rep + 16) = (WORD)g_PtrX;
        *(WORD *)(rep + 18) = (WORD)g_PtrY;
        *(WORD *)(rep + 20) = (WORD)g_PtrX;
        *(WORD *)(rep + 22) = (WORD)g_PtrY;
        SendTo(c, rep, 32);
        break;
    }
    case X_TranslateCoordinates: {
        BYTE rep[32];
        ReplyHead(rep, c, 0);
        rep[1] = 1;
        if (bodyLen >= 12) {
            *(WORD *)(rep + 12) = *(const WORD *)(body + 8);
            *(WORD *)(rep + 14) = *(const WORD *)(body + 10);
        }
        SendTo(c, rep, 32);
        break;
    }
    case X_GrabPointer: {
        BYTE rep[32];
        ReplyHead(rep, c, 0);
        rep[1] = 0;                                                 // Success
        g_PtrGrab = c;
        g_PtrGrabWin = (bodyLen >= 4) ? *(const DWORD *)(body + 0) : ID_ROOT;
        g_PtrGrabExplicit = TRUE;
        SendTo(c, rep, 32);
        break;
    }
    case X_GrabKey: {
        BYTE rep[32];
        ReplyHead(rep, c, 0);
        rep[1] = 0;
        SendTo(c, rep, 32);
        break;
    }
    case X_UngrabPointer:
        g_PtrGrab = NULL; g_PtrGrabExplicit = FALSE;
        break;
    case X_GrabButton:
        if (bodyLen >= 18 && g_NumBtnGrabs < MAX_GRABS) {
            g_BtnGrabs[g_NumBtnGrabs].c = c;
            g_BtnGrabs[g_NumBtnGrabs].win = *(const DWORD *)(body + 0);
            g_BtnGrabs[g_NumBtnGrabs].button = body[16];
            g_NumBtnGrabs++;
        }
        break;
    case X_UngrabButton: {
        // button in header data byte, grab-window at body+0. Remove ONLY the matching
        // grab -- not every grab this client holds (that killed 9wm's root menu grab).
        if (bodyLen >= 4) {
            XID win = *(const DWORD *)(body + 0);
            int button = h->data;                          // 0 = AnyButton
            int i, j;
            for (i = 0, j = 0; i < g_NumBtnGrabs; i++) {
                if (g_BtnGrabs[i].c == c && g_BtnGrabs[i].win == win &&
                    (button == 0 || g_BtnGrabs[i].button == 0 || g_BtnGrabs[i].button == button))
                    continue;                              // drop this one
                g_BtnGrabs[j++] = g_BtnGrabs[i];
            }
            g_NumBtnGrabs = j;
        }
        break;
    }
    case 97: { /* QueryBestSize: class@data, drawable@0, w@4, h@6 -> best w,h.
                  Cursors are 1:1 on the GDI side; echo the requested size (or
                  a sane 16x16 default) so XQueryBestCursor/XCreateFontCursor
                  succeed instead of erroring. */
        BYTE rep[32];
        WORD w = (bodyLen >= 8) ? *(const WORD *)(body + 4) : 16;
        WORD hh = (bodyLen >= 8) ? *(const WORD *)(body + 6) : 16;
        RtlZeroMemory(rep, sizeof(rep));
        ReplyHead(rep, c, 0);
        if (w == 0) w = 16;
        if (hh == 0) hh = 16;
        *(WORD *)(rep + 8) = w;
        *(WORD *)(rep + 10) = hh;
        SendTo(c, rep, 32);
        break;
    }
    case 101: { /* GetKeyboardMapping: first-keycode@0, count@1 -> per-keycode
                   keysym lists.  Every Xt/Motif client needs this (Xlib caches
                   it for XLookupString; a NULL map crashes _XtBuildKeysymTables). */
        BYTE first = (bodyLen >= 1) ? body[0] : 8;
        BYTE count = (bodyLen >= 2) ? body[1] : 0;
        const int perKc = 2;                     /* keysyms per keycode (base, shift) */
        DWORD nsyms = (DWORD)count * perKc;
        DWORD replen = 32 + nsyms * 4;
        BYTE *rep = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, replen);
        int i;
        if (!rep) break;
        ReplyHead(rep, c, nsyms);                /* reply-length = nsyms 4-byte units */
        rep[1] = (BYTE)perKc;                    /* keysyms_per_keycode */
        for (i = 0; i < count; i++) {
            BYTE kc = (BYTE)(first + i);
            *(DWORD *)(rep + 32 + (i * perKc + 0) * 4) = KeycodeToKeysym(kc, 0);
            *(DWORD *)(rep + 32 + (i * perKc + 1) * 4) = KeycodeToKeysym(kc, 1);
        }
        SendTo(c, rep, replen);
        HeapFree(GetProcessHeap(), 0, rep);
        break;
    }
    case 119: { /* GetModifierMapping: 8 modifier sets (Shift,Lock,Control,
                   Mod1..Mod5), keycodes_per_modifier each.  Keycodes are VK
                   codes (matching our KeyPress delivery). */
        const int perMod = 2;
        DWORD replen = 32 + 8 * perMod;
        BYTE rep[32 + 8 * 2];
        RtlZeroMemory(rep, sizeof(rep));
        ReplyHead(rep, c, 2 * perMod);           /* data = 8*perMod bytes = 2*perMod units */
        rep[1] = (BYTE)perMod;                    /* keycodes_per_modifier */
        rep[32 + 0 * perMod] = 0x10;              /* Shift   -> VK_SHIFT */
        rep[32 + 1 * perMod] = 0x14;              /* Lock    -> VK_CAPITAL */
        rep[32 + 2 * perMod] = 0x11;              /* Control -> VK_CONTROL */
        rep[32 + 3 * perMod] = 0x12;              /* Mod1    -> VK_MENU (Alt) */
        SendTo(c, rep, replen);                    /* Mod2..Mod5 left empty */
        break;
    }
    case 23: { /* GetSelectionOwner: selection ATOM@0 -> owner WINDOW.  We have
                  no selection/clipboard model yet; report None (unowned), which
                  is a valid answer Motif's clipboard init tolerates. */
        BYTE rep[32];
        RtlZeroMemory(rep, sizeof(rep));
        ReplyHead(rep, c, 0);
        *(DWORD *)(rep + 8) = 0;                   /* owner = None */
        SendTo(c, rep, 32);
        break;
    }
    case 24: { /* ConvertSelection: requestor@0, selection@4, target@8,
                  property@12, time@16.  No owner exists, so per ICCCM the server
                  sends the requestor a SelectionNotify with property=None.
                  Without this, Motif's clipboard/DND init blocks forever waiting
                  on XtGetSelectionValue -> widgets never realize (blank shell). */
        BYTE ev[32];
        RtlZeroMemory(ev, sizeof(ev));
        ev[0] = 31;                                /* SelectionNotify */
        ev[2] = (BYTE)(c->sequence & 0xFF);
        ev[3] = (BYTE)((c->sequence >> 8) & 0xFF);
        if (bodyLen >= 20) {
            *(DWORD *)(ev + 4)  = *(const DWORD *)(body + 16);  /* time */
            *(DWORD *)(ev + 8)  = *(const DWORD *)(body + 0);   /* requestor */
            *(DWORD *)(ev + 12) = *(const DWORD *)(body + 4);   /* selection */
            *(DWORD *)(ev + 16) = *(const DWORD *)(body + 8);   /* target */
        }
        *(DWORD *)(ev + 20) = 0;                   /* property = None (refused) */
        SendTo(c, ev, 32);
        break;
    }
    case X_QueryFont: {
        BYTE rep[60];
        RtlZeroMemory(rep, sizeof(rep));
        rep[0] = 1;
        rep[2] = (BYTE)(c->sequence & 0xFF);
        rep[3] = (BYTE)((c->sequence >> 8) & 0xFF);
        *(DWORD *)(rep + 4) = 7;
        // min == max bounds (fixed font): rbearing, width, ascent, descent (measured).
        *(WORD *)(rep + 10) = (WORD)g_FontW; *(WORD *)(rep + 12) = (WORD)g_FontW;
        *(WORD *)(rep + 14) = (WORD)g_FontAsc; *(WORD *)(rep + 16) = (WORD)g_FontDesc;
        *(WORD *)(rep + 26) = (WORD)g_FontW; *(WORD *)(rep + 28) = (WORD)g_FontW;
        *(WORD *)(rep + 30) = (WORD)g_FontAsc; *(WORD *)(rep + 32) = (WORD)g_FontDesc;
        *(WORD *)(rep + 40) = 32; *(WORD *)(rep + 42) = 255; *(WORD *)(rep + 44) = 32;
        rep[51] = 1;
        *(WORD *)(rep + 52) = (WORD)g_FontAsc; *(WORD *)(rep + 54) = (WORD)g_FontDesc;
        SendTo(c, rep, 60);
        break;
    }
    case X_ListFonts: {
        // Every pattern matches the one canonical fixed font, reported with a
        // full XLFD so charset parsing (fontset matching, Xlib i18n) works.
        static const char fname[] =
            "-misc-fixed-medium-r-normal--13-120-75-75-c-60-iso8859-1";
        BYTE rep[32 + 60];
        int n = (int)(sizeof(fname) - 1);           /* 57 */
        int pad = (1 + n + 3) & ~3;                 /* 60 */
        RtlZeroMemory(rep, sizeof(rep));
        ReplyHead(rep, c, pad / 4);
        *(WORD *)(rep + 8) = 1;                     /* nFonts */
        rep[32] = (BYTE)n;                          /* LISTofSTR: len, chars */
        RtlCopyMemory(rep + 33, fname, n);
        SendTo(c, rep, 32 + pad);
        break;
    }
    case X_AllocColor: {
        BYTE rep[32];
        ReplyHead(rep, c, 0);
        if (bodyLen >= 10) {
            WORD r = *(const WORD *)(body + 4), g = *(const WORD *)(body + 6), b = *(const WORD *)(body + 8);
            *(WORD *)(rep + 8) = r; *(WORD *)(rep + 10) = g; *(WORD *)(rep + 12) = b;
            *(DWORD *)(rep + 16) = ((DWORD)(r >> 8) << 16) | ((DWORD)(g >> 8) << 8) | (b >> 8);
        }
        SendTo(c, rep, 32);
        break;
    }
    case X_AllocNamedColor: {
        /* cmap@0, nbytes@4, name@8 -> pixel + exact/screen RGB */
        BYTE rep[32];
        WORD r = 0xFFFF, g = 0xFFFF, b = 0xFFFF;
        ReplyHead(rep, c, 0);
        if (bodyLen >= 8) {
            int n = *(const WORD *)(body + 4);
            if (n > bodyLen - 8) n = 0;         /* clamp to request */
            LookupColorName((const char *)(body + 8), n, &r, &g, &b);
        }
        *(DWORD *)(rep + 8) = ((DWORD)(r >> 8) << 16) | ((DWORD)(g >> 8) << 8) | (b >> 8);
        *(WORD *)(rep + 12) = r; *(WORD *)(rep + 14) = g; *(WORD *)(rep + 16) = b;
        *(WORD *)(rep + 18) = r; *(WORD *)(rep + 20) = g; *(WORD *)(rep + 22) = b;
        SendTo(c, rep, 32);
        break;
    }
    case 91: { /* QueryColors: cmap@0, pixels@4.. -> RGB per pixel (our
                  pixels are literal 0x00RRGGBB, so just unpack them) */
        int n = (bodyLen >= 4) ? (int)((bodyLen - 4) / 4) : 0;
        int i;
        BYTE *rep;
        DWORD replen = 32 + (DWORD)n * 8;
        rep = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, replen);
        if (!rep) break;
        ReplyHead(rep, c, (n * 8) / 4);
        *(WORD *)(rep + 8) = (WORD)n;
        for (i = 0; i < n; i++) {
            DWORD pix = *(const DWORD *)(body + 4 + i * 4);
            BYTE r = (BYTE)(pix >> 16), g = (BYTE)(pix >> 8), b = (BYTE)pix;
            *(WORD *)(rep + 32 + i * 8 + 0) = (WORD)(r << 8 | r);
            *(WORD *)(rep + 32 + i * 8 + 2) = (WORD)(g << 8 | g);
            *(WORD *)(rep + 32 + i * 8 + 4) = (WORD)(b << 8 | b);
        }
        SendTo(c, rep, replen);
        HeapFree(GetProcessHeap(), 0, rep);
        break;
    }
    case X_LookupColor: {
        /* cmap@0, nbytes@4, name@8 -> exact + visual RGB (no allocation) */
        BYTE rep[32];
        WORD r = 0xFFFF, g = 0xFFFF, b = 0xFFFF;
        ReplyHead(rep, c, 0);
        if (bodyLen >= 8) {
            int n = *(const WORD *)(body + 4);
            if (n > bodyLen - 8) n = 0;
            LookupColorName((const char *)(body + 8), n, &r, &g, &b);
        }
        *(WORD *)(rep + 8)  = r; *(WORD *)(rep + 10) = g; *(WORD *)(rep + 12) = b;
        *(WORD *)(rep + 14) = r; *(WORD *)(rep + 16) = g; *(WORD *)(rep + 18) = b;
        SendTo(c, rep, 32);
        break;
    }
    case X_QueryExtension: {
        BYTE rep[32];
        ReplyHead(rep, c, 0);
        SendTo(c, rep, 32);                                         // present=False
        break;
    }
    case X_CreateGC: {
        if (bodyLen >= 12) {
            XID gid = *(const DWORD *)(body + 0);
            DWORD mask = *(const DWORD *)(body + 8);
            const DWORD *vals = (const DWORD *)(body + 12);
            DWORD vcount = (bodyLen - 12) / 4;
            XGCObj *g = FindGC(gid);
            if (!g) {
                g = (XGCObj *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(XGCObj));
                if (g) { g->id = gid; g->bg = WHITE_PIXEL; g->function = GXcopy; g->next = g_GCs; g_GCs = g; }
            }
            if (g) {
                if (mask & GCFunction) g->function = ValueFor(mask, GCFunction, vals, vcount);
                if (mask & GCForeground) g->fg = ValueFor(mask, GCForeground, vals, vcount);
                if (mask & GCBackground) g->bg = ValueFor(mask, GCBackground, vals, vcount);
                if (mask & GCLineWidth) g->lineWidth = ValueFor(mask, GCLineWidth, vals, vcount);
            }
        }
        break;
    }
    case X_ChangeGC: {
        if (bodyLen >= 8) {
            XGCObj *g = FindGC(*(const DWORD *)(body + 0));
            DWORD mask = *(const DWORD *)(body + 4);
            const DWORD *vals = (const DWORD *)(body + 8);
            DWORD vcount = (bodyLen - 8) / 4;
            if (g) {
                if (mask & GCFunction) g->function = ValueFor(mask, GCFunction, vals, vcount);
                if (mask & GCForeground) g->fg = ValueFor(mask, GCForeground, vals, vcount);
                if (mask & GCBackground) g->bg = ValueFor(mask, GCBackground, vals, vcount);
                if (mask & GCLineWidth) g->lineWidth = ValueFor(mask, GCLineWidth, vals, vcount);
            }
        }
        break;
    }
    case X_CopyGC: {
        if (bodyLen >= 12) {
            XGCObj *s = FindGC(*(const DWORD *)(body + 0));
            XGCObj *d = FindGC(*(const DWORD *)(body + 4));
            DWORD mask = *(const DWORD *)(body + 8);
            if (s && d) {
                if (mask & GCFunction) d->function = s->function;
                if (mask & GCForeground) d->fg = s->fg;
                if (mask & GCBackground) d->bg = s->bg;
                if (mask & GCLineWidth) d->lineWidth = s->lineWidth;
            }
        }
        break;
    }
    case X_FreeGC: {
        XID gid = *(const DWORD *)body;
        XGCObj **pp;
        for (pp = &g_GCs; *pp; pp = &(*pp)->next)
            if ((*pp)->id == gid) { XGCObj *g = *pp; *pp = g->next; HeapFree(GetProcessHeap(), 0, g); break; }
        break;
    }
    case X_CreatePixmap: {
        // depth(data) pid(4) drawable(4) w(2) h(2)
        if (bodyLen >= 12) {
            XID pid = *(const DWORD *)(body + 0);
            WORD ww = *(const WORD *)(body + 8), hh = *(const WORD *)(body + 10);
            XPixmap *p = (XPixmap *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(XPixmap));
            if (p) {
                BITMAPINFO bmi;
                HDC dc = GetDC(g_Hwnd);
                RtlZeroMemory(&bmi, sizeof(bmi));
                bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bmi.bmiHeader.biWidth = ww ? ww : 1;
                bmi.bmiHeader.biHeight = -(LONG)(hh ? hh : 1);
                bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32;
                bmi.bmiHeader.biCompression = BI_RGB;
                p->id = pid; p->w = ww; p->h = hh;
                p->dc = CreateCompatibleDC(dc);
                p->bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &p->bits, NULL, 0);
                SelectObject(p->dc, p->bmp);
                SelectObject(p->dc, GetStockObject(ANSI_FIXED_FONT));
                ReleaseDC(g_Hwnd, dc);
                p->next = g_Pixmaps; g_Pixmaps = p;
            }
        }
        break;
    }
    case X_FreePixmap: {
        XID pid = *(const DWORD *)body;
        XPixmap **pp;
        for (pp = &g_Pixmaps; *pp; pp = &(*pp)->next)
            if ((*pp)->id == pid) {
                XPixmap *p = *pp; *pp = p->next;
                DeleteDC(p->dc); DeleteObject(p->bmp);
                HeapFree(GetProcessHeap(), 0, p);
                break;
            }
        break;
    }
    case X_ClearArea: {
        if (bodyLen >= 12) {
            XWin *w = FindWin(*(const DWORD *)(body + 0));
            if (w) DoClearArea(w, *(const SHORT *)(body + 4), *(const SHORT *)(body + 6),
                               *(const WORD *)(body + 8), *(const WORD *)(body + 10));
        }
        break;
    }
    case X_CopyArea: {
        if (bodyLen >= 24) {
            XID src = *(const DWORD *)(body + 0), dst = *(const DWORD *)(body + 4);
            XGCObj *g = FindGC(*(const DWORD *)(body + 8));
            SHORT sx = *(const SHORT *)(body + 12), sy = *(const SHORT *)(body + 14);
            SHORT dx = *(const SHORT *)(body + 16), dy = *(const SHORT *)(body + 18);
            WORD w = *(const WORD *)(body + 20), h = *(const WORD *)(body + 22);
            DoCopyArea(src, dst, g, sx, sy, w, h, dx, dy);
        }
        break;
    }
    case X_PolyFillRectangle: case X_PolyRectangle: {
        if (bodyLen >= 8) {
            XTarget t; ResolveDrawable(*(const DWORD *)(body + 0), &t);
            if (t.valid) {
                XGCObj *g = FindGC(*(const DWORD *)(body + 4));
                if (h->opcode == X_PolyFillRectangle)
                    DrawFillRects(&t, g, (const X_RECT *)(body + 8), (bodyLen - 8) / 8);
                else
                    DrawFrameRects(&t, g, (const X_RECT *)(body + 8), (bodyLen - 8) / 8);
            }
        }
        break;
    }
    case X_PolySegment: {
        if (bodyLen >= 8) {
            XTarget t; ResolveDrawable(*(const DWORD *)(body + 0), &t);
            if (t.valid) DrawSegments(&t, FindGC(*(const DWORD *)(body + 4)),
                                      (const X_SEGMENT *)(body + 8), (bodyLen - 8) / 8);
        }
        break;
    }
    case X_PolyLine: {
        if (bodyLen >= 8) {
            XTarget t; ResolveDrawable(*(const DWORD *)(body + 0), &t);
            if (t.valid) DrawPolyline(&t, FindGC(*(const DWORD *)(body + 4)),
                                      (const X_POINT *)(body + 8), (bodyLen - 8) / 4, h->data);
        }
        break;
    }
    case X_FillPoly: {
        if (bodyLen >= 12) {
            XTarget t; ResolveDrawable(*(const DWORD *)(body + 0), &t);
            if (t.valid) DrawFillPoly(&t, FindGC(*(const DWORD *)(body + 4)),
                                      (const X_POINT *)(body + 12), (bodyLen - 12) / 4);
        }
        break;
    }
    case X_PolyFillArc: case X_PolyArc: {
        if (bodyLen >= 8) {
            XTarget t; ResolveDrawable(*(const DWORD *)(body + 0), &t);
            if (t.valid) DrawFillArcs(&t, FindGC(*(const DWORD *)(body + 4)),
                                      (const X_ARC *)(body + 8), (bodyLen - 8) / 12);
        }
        break;
    }
    case X_ImageText8: {
        if (bodyLen >= 12) {
            XTarget t; ResolveDrawable(*(const DWORD *)(body + 0), &t);
            if (t.valid) {
                int len = h->data;
                if ((DWORD)(12 + len) <= bodyLen)
                    DrawText8(&t, FindGC(*(const DWORD *)(body + 4)),
                              *(const SHORT *)(body + 8), *(const SHORT *)(body + 10),
                              (const char *)(body + 12), len);
            }
        }
        break;
    }
    case X_PolyText8: {
        // drawable(4) gc(4) x(2) y(2) then TEXTITEM8 list: [len][delta][string] items,
        // or [255][font:4] font-shifts (skipped). XDrawString -> this (transparent glyphs).
        if (bodyLen >= 12) {
            XID drw = *(const DWORD *)(body + 0);
            XGCObj *g = FindGC(*(const DWORD *)(body + 4));
            SHORT y = *(const SHORT *)(body + 10);
            int cx = *(const SHORT *)(body + 8);
            const BYTE *p = body + 12, *end = body + bodyLen;
            while (p + 2 <= end) {
                BYTE m = p[0];
                signed char delta;
                XTarget t;
                if (m == 255) { p += 5; continue; }        // font shift
                if (m == 0) break;                         // padding/end
                if (p + 2 + m > end) break;
                delta = (signed char)p[1];
                cx += delta;
                ResolveDrawable(drw, &t);
                if (t.valid) DrawTextGeneric(&t, g, (SHORT)cx, y, (const char *)(p + 2), m, FALSE);
                cx += m * g_FontW;
                p += 2 + m;
            }
        }
        break;
    }
    case X_PutImage: {
        // format(data) drawable(4) gc(4) w(2) h(2) dstX(2) dstY(2) leftPad(1) depth(1) pad(2) image
        if (bodyLen >= 20) {
            XTarget t; ResolveDrawable(*(const DWORD *)(body + 0), &t);
            if (t.valid)
                DrawPutImage(&t, *(const SHORT *)(body + 12), *(const SHORT *)(body + 14),
                             *(const WORD *)(body + 8), *(const WORD *)(body + 10),
                             body[17], body + 20, bodyLen - 20);
        }
        break;
    }
    default:
        // No-reply requests can be safely ignored (already framed by length),
        // but a REPLY-BEARING request we don't implement would leave the
        // client blocked in _XReply forever.  Real servers answer those with
        // an error; BadImplementation makes Xlib report the exact opcode on
        // the client's stderr instead of hanging (this is how dthello's
        // missing-request hang was made visible).
        if (IsReplyOp(h->opcode)) {
            BYTE err[32];
            RtlZeroMemory(err, sizeof(err));
            err[0] = 0;                                     /* Error */
            err[1] = 17;                                    /* BadImplementation */
            err[2] = (BYTE)(c->sequence & 0xFF);
            err[3] = (BYTE)((c->sequence >> 8) & 0xFF);
            err[10] = h->opcode;                            /* major opcode */
            SendTo(c, err, 32);
            XDBG("psxx11: unimplemented reply op=%u -> BadImplementation\n",
                 h->opcode);
        }
        break;
    }
}

/* ================================================================================
 *  Per-client reader thread
 * ================================================================================ */
static void FreeClient(XClient *c)
{
    XWin *w;
    int i, j;
    // drop this client's selectors from every window
    for (w = g_AllWins; w; w = w->hnext) {
        for (i = 0, j = 0; i < w->numSel; i++)
            if (w->sel[i].client != c) w->sel[j++] = w->sel[i];
        w->numSel = j;
    }
    // drop this client's grabs
    if (g_PtrGrab == c) { g_PtrGrab = NULL; g_PtrGrabExplicit = FALSE; }
    for (i = 0, j = 0; i < g_NumBtnGrabs; i++)
        if (g_BtnGrabs[i].c != c) g_BtnGrabs[j++] = g_BtnGrabs[i];
    g_NumBtnGrabs = j;
    XDBG("psxx11: client %lu gone\n", c->id);
    CancelIo(c->pipe);
    FlushFileBuffers(c->pipe);
    DisconnectNamedPipe(c->pipe);
    CloseHandle(c->pipe);
    if (c->rev) CloseHandle(c->rev);
    if (c->wev) CloseHandle(c->wev);
    if (c->inBuf) HeapFree(GetProcessHeap(), 0, c->inBuf);
    if (c->outBuf) HeapFree(GetProcessHeap(), 0, c->outBuf);
    HeapFree(GetProcessHeap(), 0, c);
}

// Unlink and free any clients that died this iteration.
static void ReapDead(void)
{
    XClient **pp = &g_Clients;
    while (*pp) {
        XClient *c = *pp;
        if (!c->alive) { *pp = c->next; FreeClient(c); }
        else pp = &c->next;
    }
}

/* ---- accept: one pending pipe instance with an overlapped ConnectNamedPipe ---- */
static HANDLE     g_AcceptPipe = INVALID_HANDLE_VALUE;
static OVERLAPPED g_AcceptOv;
static HANDLE     g_AcceptEvent;
static void StartAccept(void);

// Promote the just-connected accept pipe into a live client and start its first read.
static void FinalizeClient(void)
{
    XClient *c = (XClient *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(XClient));
    if (!c) { CloseHandle(g_AcceptPipe); g_AcceptPipe = INVALID_HANDLE_VALUE; return; }
    c->pipe = g_AcceptPipe;
    c->id = ++g_ClientSeq;
    c->alive = TRUE;
    c->state = CS_HANDSHAKE;
    c->idBase = ID_CLIENT_BASE + (c->id - 1) * ID_CLIENT_STRIDE;
    c->idMask = ID_CLIENT_MASK;
    c->rev = CreateEventW(NULL, TRUE, FALSE, NULL);     // manual-reset overlapped events
    c->wev = CreateEventW(NULL, TRUE, FALSE, NULL);
    c->next = g_Clients; g_Clients = c;
    g_AcceptPipe = INVALID_HANDLE_VALUE;
    XDBG("psxx11: client %lu connected\n", c->id);
    IssueRead(c);
}

// Create a fresh pipe instance and (overlapped) wait for the next connection.
static void StartAccept(void)
{
    DWORD e;
    ResetEvent(g_AcceptEvent);
    RtlZeroMemory(&g_AcceptOv, sizeof(g_AcceptOv));
    g_AcceptOv.hEvent = g_AcceptEvent;
    g_AcceptPipe = CreateNamedPipeW(PSX_X11_PIPE_NAME,
                                    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                    PIPE_UNLIMITED_INSTANCES, 0x10000, 0x10000, 0, NULL);
    if (g_AcceptPipe == INVALID_HANDLE_VALUE) return;
    if (ConnectNamedPipe(g_AcceptPipe, &g_AcceptOv)) { FinalizeClient(); StartAccept(); return; }
    e = GetLastError();
    if (e == ERROR_PIPE_CONNECTED) { FinalizeClient(); StartAccept(); }
    else if (e != ERROR_IO_PENDING) { CloseHandle(g_AcceptPipe); g_AcceptPipe = INVALID_HANDLE_VALUE; }
    // else pending -> g_AcceptEvent signals on connect
}

static void OnAcceptDone(void)
{
    DWORD n;
    if (GetOverlappedResult(g_AcceptPipe, &g_AcceptOv, &n, FALSE) ||
        GetLastError() == ERROR_PIPE_CONNECTED)
        FinalizeClient();
    else { CloseHandle(g_AcceptPipe); g_AcceptPipe = INVALID_HANDLE_VALUE; }
    StartAccept();
}

/* ================================================================================
 *  Win32 window + repaint
 * ================================================================================ */
static void BlitScreen(void)
{
    HDC dc;
    if (!g_Hwnd) return;
    dc = GetDC(g_Hwnd);
    if (dc) {
        BitBlt(dc, 0, 0, SCREEN_W, SCREEN_H, g_ScreenDc, 0, 0, SRCCOPY);
        ReleaseDC(g_Hwnd, dc);
    }
}

static void CreateScreen(HWND hwnd)
{
    HDC dc = GetDC(hwnd);
    BITMAPINFO bmi;
    RECT r = { 0, 0, SCREEN_W, SCREEN_H };
    RtlZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = SCREEN_W;
    bmi.bmiHeader.biHeight = -SCREEN_H;
    bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    g_ScreenDc = CreateCompatibleDC(dc);
    g_ScreenDib = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &g_ScreenBits, NULL, 0);
    SelectObject(g_ScreenDc, g_ScreenDib);
    // A fixed-pitch font, and measure it so QueryFont reports what GDI actually draws.
    {
        TEXTMETRICA tm;
        SelectObject(g_ScreenDc, GetStockObject(ANSI_FIXED_FONT));
        if (GetTextMetricsA(g_ScreenDc, &tm)) {
            g_FontW = tm.tmAveCharWidth;
            g_FontAsc = tm.tmAscent;
            g_FontDesc = tm.tmDescent;
            g_FontH = tm.tmHeight;
        }
    }
    FillRect(g_ScreenDc, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));   // root background
    ReleaseDC(hwnd, dc);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        CreateScreen(hwnd);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        BitBlt(dc, 0, 0, SCREEN_W, SCREEN_H, g_ScreenDc, 0, 0, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }
    // Input runs on the same (only) thread as request dispatch, so no locking.
    case WM_MOUSEMOVE:
        g_PtrX = (short)LOWORD(lp); g_PtrY = (short)HIWORD(lp);
        UpdateCrossing(g_PtrX, g_PtrY);
        RoutePointer(MotionNotify, 0, PointerMotionMask, g_PtrX, g_PtrY, FALSE, FALSE);
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        g_PtrX = (short)LOWORD(lp); g_PtrY = (short)HIWORD(lp);
        SetCapture(hwnd);
        RoutePointer(ButtonPress, (msg == WM_LBUTTONDOWN) ? 1 : (msg == WM_MBUTTONDOWN) ? 2 : 3,
                     ButtonPressMask, g_PtrX, g_PtrY, TRUE, FALSE);
        return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
        g_PtrX = (short)LOWORD(lp); g_PtrY = (short)HIWORD(lp);
        ReleaseCapture();
        RoutePointer(ButtonRelease, (msg == WM_LBUTTONUP) ? 1 : (msg == WM_MBUTTONUP) ? 2 : 3,
                     ButtonReleaseMask, g_PtrX, g_PtrY, FALSE, TRUE);
        return 0;
    case WM_KEYDOWN:
        DeliverInput(KeyPress, (BYTE)wp, KeyPressMask, g_PtrX, g_PtrY);
        return 0;
    case WM_KEYUP:
        DeliverInput(KeyRelease, (BYTE)wp, KeyReleaseMask, g_PtrX, g_PtrY);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSEXW wc;
    MSG msg;
    RECT rc = { 0, 0, SCREEN_W, SCREEN_H };

    (void)prev; (void)cmd; (void)show;

    RtlZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"PsxX11Server";
    RegisterClassExW(&wc);

    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_Hwnd = CreateWindowExW(0, L"PsxX11Server", L"ReactOS X Server :0",
                             WS_OVERLAPPEDWINDOW, 40, 40,
                             rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, inst, NULL);
    if (!g_Hwnd) return 1;
    ShowWindow(g_Hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_Hwnd);
    UpdateWindow(g_Hwnd);
    XDBG("psxx11: server up, hwnd=%p\n", (void *)g_Hwnd);

    // The root window covers the screen.
    g_Root = NewWin(ID_ROOT, NULL, 0, 0, SCREEN_W, SCREEN_H);
    if (g_Root) { g_Root->mapped = TRUE; g_Root->hasBg = TRUE; g_Root->bgPixel = 0x00808080; }

    // Single-threaded event loop: one thread services client pipe I/O (overlapped) and
    // Win32 input together via MsgWaitForMultipleObjects. No locks, no per-client threads.
    g_AcceptEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    StartAccept();

    for (;;) {
        HANDLE   handles[MAXIMUM_WAIT_OBJECTS];
        DWORD    n = 0;
        XClient *c, *cnext;

        handles[n++] = g_AcceptEvent;
        for (c = g_Clients; c && n + 2 <= MAXIMUM_WAIT_OBJECTS; c = c->next) {
            handles[n++] = c->rev;
            if (c->writePending) handles[n++] = c->wev;
        }

        // Block until SOMETHING is ready (or 33ms). We don't act on the returned index --
        // MsgWaitForMultipleObjects reports only the lowest signaled handle, which would
        // let a client whose events stay signaled (an animating app flooding requests)
        // permanently starve older clients + the Win32 message queue. Instead, after the
        // wake we service EVERY ready handle below.
        MsgWaitForMultipleObjectsEx(n, handles, 33, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        if (WaitForSingleObject(g_AcceptEvent, 0) == WAIT_OBJECT_0)
            OnAcceptDone();
        for (c = g_Clients; c; c = cnext) {
            cnext = c->next;                     // c may be freed by ReapDead later
            if (WaitForSingleObject(c->rev, 0) == WAIT_OBJECT_0)
                OnReadDone(c);
            if (c->alive && c->writePending && WaitForSingleObject(c->wev, 0) == WAIT_OBJECT_0)
                OnWriteDone(c);
        }
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        ReapDead();
        if (InterlockedExchange(&g_Dirty, 0)) BlitScreen();
    }
}
