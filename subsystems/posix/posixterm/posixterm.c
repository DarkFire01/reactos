/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     posixterm -- POSIX controlling-terminal / session host. A Win32
 *              console alternative to NT 4.0 posix.exe: it registers with psxss
 *              as the session leader, asks psxss to spawn the POSIX child
 *              (default: /bin/sh), and bridges that child's tty read/write/
 *              isatty/tcsetattr/exit over LPC -- driving the Win32 console
 *              through John Miller's permissive vt100.c/console.c VT100 emulator.
 *
 *              Speaks the SAME MS session-port ABI our psxss + the real psxdll
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */
#include <stdarg.h>
#include <stdio.h>

#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS

#include <ndk/rtlfuncs.h>
#include <ndk/lpcfuncs.h>
#include <ndk/mmfuncs.h>
#include <ndk/obfuncs.h>

#include <subsys/sm/smmsg.h>    /* SmConnectToSm / SmLoadDeferedSubsystem */

#include "vt100.h"

/*======================================================================
 * Wire format
 *
 * Every message is a PORT_MESSAGE (0x18 header) + body. We index the body
 * as ULONGs starting at message offset 0x18, so u[i] == message byte 0x18+4i:
 *
 *   On \PSXSS\PSXSES\P<pid> (our own server port):
 *     u[0]=0x18 API selector   u[1]=0x1C errno   u[2]=0x20 req[0] (sub-op)
 *     u[3]=0x24 req[1]         u[4]=0x28 req[2]  u[5]=0x2C req[3]
 *   On \PSXSS\SESPORT (we are the client):
 *     u[0]=0x18 context   u[1]=0x1C discriminator   ...spawn fields below.
 *====================================================================*/
typedef struct _SES_MSG
{
    PORT_MESSAGE Header;    /* 0x18 */
    ULONG        u[25];     /* body: u[i] == msg+0x18+4*i */
} SES_MSG, *PSES_MSG;

/* Body ULONG indices (msg offset = 0x18 + 4*index). */
#define IDX_SELECTOR   0    /* 0x18 */
#define IDX_ERRNO      1    /* 0x1C */
#define IDX_REQ0       2    /* 0x20 */
#define IDX_REQ1       3    /* 0x24 */
#define IDX_REQ2       4    /* 0x28 */
#define IDX_REQ3       5    /* 0x2C */
/* SESPORT create-process fields */
#define IDX_STATUS     2    /* 0x20 (reply) */
#define IDX_DISCRIM    1    /* 0x1C */
#define IDX_LEADERPID  4    /* 0x28 */
#define IDX_SESSIONID  5    /* 0x2C */
#define IDX_FDCOUNT    7    /* 0x34 */
#define IDX_IMGOFF     8    /* 0x38 */
#define IDX_CWDOFF     9    /* 0x3C */
#define IDX_ARGVOFF   10    /* 0x40 */
#define IDX_ENVOFF    11    /* 0x44 */
#define IDX_SHAREDBASE 12   /* 0x48 */

/* PORT_MESSAGE.u1.Length packs {TotalLength<<16 | DataLength} conceptually as
 * two USHORTs; posix.exe uses these literal encodings. */
#define SPAWN_LENGTH   0x00500038   /* DataLength 0x38, TotalLength 0x50 */

#define SECTION_SIZE   0x10000      /* 64 KiB D<pid> section */
#define INBUF_SIZE     4096

/*======================================================================
 * Globals
 *====================================================================*/
static const char *g_MyName = "posixterm";

static HANDLE  g_SectionHandle;
static PVOID   g_SharedBase;        /* D<pid> section, our view                */
static SIZE_T  g_ViewSize = SECTION_SIZE;
static HANDLE  g_SesPort;           /* client handle to \PSXSS\SESPORT          */
static HANDLE  g_LeaderPort;        /* our \PSXSS\PSXSES\P<pid> server port     */
static ULONG   g_SessionId;
static ULONG   g_MyPid;
static ULONG   g_ReplyContext;      /* echoed spawn Context, used for signals   */
static CRITICAL_SECTION g_SesLock;  /* serialize SESPORT client sends            */

static UCHAR   g_Termios[0x44];
#define T_CIFLAG(t)  (*(ULONG*)((t)+0x00))
#define T_COFLAG(t)  (*(ULONG*)((t)+0x04))
#define T_CCFLAG(t)  (*(ULONG*)((t)+0x08))
#define T_CLFLAG(t)  (*(ULONG*)((t)+0x0C))
#define LFLAG_ECHO   0x01
#define LFLAG_ICANON 0x10
#define LFLAG_ISIG   0x40
#define IFLAG_ICRNL  0x02

/* Cooked input ring buffer, filled by the input thread, drained by read(). */
static CRITICAL_SECTION g_InLock;
static HANDLE  g_InReady;           /* signalled when a byte/EOF is available   */
static char    g_InBuf[INBUF_SIZE];
static int     g_InHead, g_InTail;  /* [head,tail) pending bytes                */
static BOOL    g_Eof;

static HANDLE  g_hConIn;

static void SendTtySignal(ULONG code);   /* fwd: used by the input thread */

/*======================================================================
 * Small helpers
 *====================================================================*/
static void Trace(const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(b, sizeof(b) - 1, fmt, ap);
    b[sizeof(b) - 1] = 0;
    va_end(ap);
    OutputDebugStringA(b);
}

static ULONG InBufCount(void)
{
    return (g_InTail - g_InHead + INBUF_SIZE) % INBUF_SIZE;
}

static void InBufPush(char c)
{
    EnterCriticalSection(&g_InLock);
    if (((g_InTail + 1) % INBUF_SIZE) != g_InHead)
    {
        g_InBuf[g_InTail] = c;
        g_InTail = (g_InTail + 1) % INBUF_SIZE;
    }
    LeaveCriticalSection(&g_InLock);
    SetEvent(g_InReady);
}

/*======================================================================
 * VT100 output -- feed a child write() straight into the emulator.
 *====================================================================*/
static void EmitToConsole(const char *buf, ULONG len)
{
    vtProcessedTextOut((char *)buf, (int)len);
}

/*======================================================================
 * Input thread -- ReadConsoleInput -> line discipline -> ring buffer.
 * Canonical (ICANON) mode: accumulate a line, echo, flush on Enter, handle
 * backspace + ^D. Raw mode: push each byte immediately.
 *====================================================================*/
static DWORD WINAPI InputThread(LPVOID Arg)
{
    INPUT_RECORD rec[32];
    DWORD n, i;
    char line[INBUF_SIZE];
    int  linelen = 0;

    UNREFERENCED_PARAMETER(Arg);

    for (;;)
    {
        if (!ReadConsoleInputA(g_hConIn, rec, 32, &n))
            break;

        for (i = 0; i < n; i++)
        {
            char ch;
            BOOL canon, echo;
            WORD rep;

            if (rec[i].EventType != KEY_EVENT || !rec[i].Event.KeyEvent.bKeyDown)
                continue;
            ch  = rec[i].Event.KeyEvent.uChar.AsciiChar;
            rep = rec[i].Event.KeyEvent.wRepeatCount;
            if (rep == 0)
                rep = 1;

            canon = (T_CLFLAG(g_Termios) & LFLAG_ICANON) != 0;
            echo  = (T_CLFLAG(g_Termios) & LFLAG_ECHO) != 0;

            if (ch == 0)
            {
                /* Non-character key: in raw mode hand editors (readline, vi)
                 * the VT100 cursor sequences; canonical mode has no use for
                 * them (matches the old drop-silently behavior). */
                const char *seq = NULL;
                switch (rec[i].Event.KeyEvent.wVirtualKeyCode)
                {
                    case VK_UP:     seq = "\x1B[A";  break;
                    case VK_DOWN:   seq = "\x1B[B";  break;
                    case VK_RIGHT:  seq = "\x1B[C";  break;
                    case VK_LEFT:   seq = "\x1B[D";  break;
                    case VK_HOME:   seq = "\x1B[H";  break;
                    case VK_END:    seq = "\x1B[F";  break;
                    case VK_DELETE: seq = "\x1B[3~"; break;
                }
                if (seq != NULL && !canon)
                {
                    while (rep--)
                    {
                        const char *s;
                        for (s = seq; *s; s++) InBufPush(*s);
                    }
                }
                continue;
            }

            if (!canon)
            {
                /* Raw mode: deliver each byte immediately (the shell's line
                 * editor drives its own echo/erase). ICRNL still applies --
                 * it's an input flag, independent of ICANON. */
                if (ch == '\r' && (T_CIFLAG(g_Termios) & IFLAG_ICRNL))
                    ch = '\n';
                while (rep--)
                {
                    if (echo) { char e[1]; e[0] = ch; EmitToConsole(e, 1); }
                    InBufPush(ch);
                }
                continue;
            }
            /* Canonical mode: key auto-repeat only matters for printables;
             * apply it there (below) and treat control keys once. */

            /* Canonical line discipline. */
            if (ch == '\r' || ch == '\n')
            {
                int k;
                if (echo) EmitToConsole("\r\n", 2);
                for (k = 0; k < linelen; k++) InBufPush(line[k]);
                InBufPush('\n');
                linelen = 0;
            }
            else if (ch == '\b' || ch == 0x7F)          /* ERASE */
            {
                if (linelen > 0)
                {
                    linelen--;
                    if (echo) EmitToConsole("\b \b", 3);
                }
            }
            else if (ch == 0x04)                        /* ^D = EOF */
            {
                int k;
                for (k = 0; k < linelen; k++) InBufPush(line[k]);
                linelen = 0;
                EnterCriticalSection(&g_InLock);
                g_Eof = TRUE;
                LeaveCriticalSection(&g_InLock);
                SetEvent(g_InReady);
            }
            else if (ch == 0x03)                        /* ^C = INTR -> SIGINT */
            {
                if (echo) EmitToConsole("^C\r\n", 4);
                linelen = 0;
                SendTtySignal(0);
            }
            else
            {
                while (rep--)
                {
                    if (linelen < (int)sizeof(line) - 1)
                    {
                        line[linelen++] = ch;
                        if (echo) { char e[1]; e[0] = ch; EmitToConsole(e, 1); }
                    }
                }
            }
        }
    }
    return 0;
}

/* Blocking cooked read: wait until data or EOF, then drain up to len bytes. */
static ULONG TtyRead(char *dst, ULONG len)
{
    ULONG got = 0;

    for (;;)
    {
        EnterCriticalSection(&g_InLock);
        while (got < len && g_InHead != g_InTail)
        {
            dst[got++] = g_InBuf[g_InHead];
            g_InHead = (g_InHead + 1) % INBUF_SIZE;
        }
        if (got > 0)
        {
            LeaveCriticalSection(&g_InLock);
            return got;
        }
        if (g_Eof)
        {
            g_Eof = FALSE;              /* consume the EOF once */
            LeaveCriticalSection(&g_InLock);
            return 0;                    /* 0 == EOF */
        }
        LeaveCriticalSection(&g_InLock);
        WaitForSingleObject(g_InReady, INFINITE);
    }
}

/*======================================================================
 * Controlling-tty signal (leader -> SESPORT, discriminator 1). Body reuse:
 * Context@0x18, discriminator@0x1C=1, pid@0x24 (== session id), code@0x28.
 * code 0=INTR ^C, 1=SUSP, 2=close, 3=QUIT.  psxss maps code->POSIX signal and
 * delivers to the session's foreground processes (PsxSesDeliverTtySignal).
 *====================================================================*/
static void SendTtySignal(ULONG code)
{
    SES_MSG msg;

    if (g_SesPort == NULL)
        return;
    RtlZeroMemory(&msg, sizeof(msg));
    msg.Header.u1.s1.TotalLength = 0x50;
    msg.Header.u1.s1.DataLength  = 0x38;
    msg.u[IDX_SELECTOR] = g_ReplyContext;   /* Context @0x18 */
    msg.u[IDX_DISCRIM]  = 1;                 /* @0x1C tty signal */
    msg.u[3]            = g_MyPid;           /* pid @0x24 == session id */
    msg.u[4]            = code;              /* code @0x28 */

    EnterCriticalSection(&g_SesLock);
    NtRequestWaitReplyPort(g_SesPort, &msg.Header, &msg.Header);
    LeaveCriticalSection(&g_SesLock);
}

/* Console Ctrl handler: Ctrl-C -> SIGINT, Ctrl-Break -> SIGTSTP, window close ->
 * the 'close' code (psxss delivers the hangup to the session). Returning TRUE
 * keeps posixterm alive (the signal goes to the POSIX child, not us). Fires when
 * the console has ENABLE_PROCESSED_INPUT (canonical + ISIG). */
static BOOL WINAPI CtrlHandler(DWORD type)
{
    if (type == CTRL_C_EVENT)     { SendTtySignal(0); return TRUE; }
    if (type == CTRL_BREAK_EVENT) { SendTtySignal(1); return TRUE; }
    if (type == CTRL_CLOSE_EVENT) { SendTtySignal(2); return TRUE; }
    return FALSE;
}

/*======================================================================
 * P<pid> server: DispatchIoRequest / DispatchExit / DispatchTcSetAttr.
 * Returns FALSE to stop the server loop (teardown).
 *====================================================================*/
static BOOL DispatchIoRequest(PSES_MSG m)
{
    ULONG subop = m->u[IDX_REQ0];
    char *shared = (char *)g_SharedBase;

    m->u[IDX_ERRNO] = 0;
    switch (subop)
    {
        case 1:     /* open controlling tty */
            m->u[IDX_REQ1] = 0;     /* pretend fd 0 */
            break;

        case 3:     /* read(fd, buf@section[0], len=req[2]) */
        {
            ULONG len = m->u[IDX_REQ2];
            ULONG got;
            if (len > SECTION_SIZE) len = SECTION_SIZE;
            got = TtyRead(shared, len);
            m->u[IDX_REQ2] = got;    /* bytes read (0 = EOF) */
            break;
        }

        case 4:     /* write(fd, buf@section[0], len=req[2]) */
        {
            ULONG len = m->u[IDX_REQ2];
            if (len > SECTION_SIZE) len = SECTION_SIZE;
            EmitToConsole(shared, len);
            m->u[IDX_REQ2] = len;    /* all bytes consumed */
            break;
        }

        case 6:
        case 7:     /* isatty */
            m->u[IDX_REQ2] = 1;      /* we are a terminal */
            break;

        default:
            m->u[IDX_ERRNO] = 22;    /* EINVAL */
            break;
    }
    return TRUE;
}

static BOOL DispatchExit(PSES_MSG m)
{
    m->u[IDX_ERRNO] = 0;
    if (m->u[IDX_REQ0] == 0)
        return FALSE;                /* teardown */
    return TRUE;
}

static BOOL DispatchTcSetAttr(PSES_MSG m)
{
    /* termios block sits at msg+0x28 == &u[IDX_REQ2]. */
    UCHAR *blk = (UCHAR *)&m->u[IDX_REQ2];

    m->u[IDX_ERRNO] = 0;
    if (m->u[IDX_REQ0] == 0)         /* tcgetattr: hand our termios out */
    {
        memcpy(blk, g_Termios, 0x44);
    }
    else if (m->u[IDX_REQ0] == 1)    /* tcsetattr: take it in, program console */
    {
        DWORD mode = 0;
        memcpy(g_Termios, blk, 0x44);
        if (T_CLFLAG(g_Termios) & LFLAG_ICANON) mode |= ENABLE_LINE_INPUT;
        if (T_CLFLAG(g_Termios) & LFLAG_ECHO)   mode |= ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT;
        if (T_CLFLAG(g_Termios) & LFLAG_ISIG)   mode |= ENABLE_PROCESSED_INPUT;
        SetConsoleMode(g_hConIn, mode);
    }
    else
    {
        m->u[IDX_ERRNO] = 22;
    }
    return TRUE;
}

static DWORD WINAPI ServerThread(LPVOID Arg)
{
    SES_MSG req, *reply = NULL;
    NTSTATUS st;
    BOOL running = TRUE;

    UNREFERENCED_PARAMETER(Arg);
    Trace("%s: P<pid> server started\n", g_MyName);

    while (running)
    {
        st = NtReplyWaitReceivePort(g_LeaderPort, NULL,
                                    reply ? &reply->Header : NULL, &req.Header);
        if (!NT_SUCCESS(st))
        {
            reply = NULL;
            continue;
        }

        switch (req.Header.u2.s2.Type & 0xFF)
        {
            case LPC_CONNECTION_REQUEST:
            {
                /* Accept both psxss's back-connect and the child's tty port.
                 * The accepted communication handle is separate from the
                 * listening port; requests still arrive here. We don't push to
                 * clients proactively, so the handle can be dropped. */
                HANDLE commPort = NULL;
                NtAcceptConnectPort(&commPort, NULL, &req.Header, TRUE, NULL, NULL);
                if (commPort) NtCompleteConnectPort(commPort);
                reply = NULL;
                continue;
            }
            case LPC_PORT_CLOSED:       /* posix.exe treats 5 as client-died */
            case LPC_CLIENT_DIED:
                reply = NULL;
                continue;
            case LPC_REQUEST:
            default:
                break;
        }

        switch (req.u[IDX_SELECTOR])
        {
            case 0:  running = DispatchIoRequest(&req);   break;
            case 1:  running = DispatchExit(&req);        break;
            case 2:  running = DispatchTcSetAttr(&req);   break;
            default: req.u[IDX_ERRNO] = 22;               break;
        }
        reply = &req;
    }

    Trace("%s: P<pid> server exiting (teardown)\n", g_MyName);
    ExitProcess(0);
    return 0;
}

/*======================================================================
 * If psxss isn't up yet (\PSXSS\PSXSES absent -> NtCreateSection returns
 * STATUS_OBJECT_PATH_NOT_FOUND), ask SM to load the deferred POSIX subsystem.
 * posixterm is a Win32 image, so merely launching it does NOT auto-load POSIX
 * the way running a POSIX image would. Faithful to posix.exe's RunPsxSs retry.
 *====================================================================*/
static void EnsurePsxSs(void)
{
    HANDLE smPort = NULL;
    UNICODE_STRING posix;
    NTSTATUS st;

    st = SmConnectToSm(NULL, NULL, 0, &smPort);
    if (!NT_SUCCESS(st) || smPort == NULL)
    {
        Trace("%s: SmConnectToSm %08x\n", g_MyName, st);
        return;
    }
    RtlInitUnicodeString(&posix, L"Posix");
    st = SmLoadDeferedSubsystem(smPort, &posix);
    Trace("%s: SmLoadDeferedSubsystem(Posix) %08x\n", g_MyName, st);
    NtClose(smPort);
}

/*======================================================================
 * Session objects: create D<pid> section + P<pid> port; start server.
 *====================================================================*/
static NTSTATUS CreateSessionObjects(void)
{
    NTSTATUS st;
    WCHAR namebuf[64];
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    LARGE_INTEGER size;
    HANDLE thread;

    /* D<pid> section. */
    _snwprintf(namebuf, 63, L"\\PSXSS\\PSXSES\\D%u", g_MyPid);
    namebuf[63] = 0;
    RtlInitUnicodeString(&name, namebuf);
    InitializeObjectAttributes(&oa, &name, 0, NULL, NULL);
    size.QuadPart = SECTION_SIZE;
    /* SEC_COMMIT (not the real posix.exe's SEC_RESERVE): commit the whole 64 KiB
     * up front so writes into the marshalling area never fault. SEC_RESERVE would
     * need per-page NtAllocateVirtualMemory(MEM_COMMIT), and ReactOS's Mm also
     * asserts on NtQueryVirtualMemory of a reserved section. The legacy posixw32
     * used SEC_COMMIT for the same reason. */
    st = NtCreateSection(&g_SectionHandle, SECTION_MAP_READ | SECTION_MAP_WRITE,
                         &oa, &size, PAGE_READWRITE, SEC_COMMIT, NULL);
    if (!NT_SUCCESS(st)) { Trace("%s: NtCreateSection(D) %08x\n", g_MyName, st); return st; }

    g_SharedBase = NULL;
    st = NtMapViewOfSection(g_SectionHandle, NtCurrentProcess(), &g_SharedBase,
                            0, 0, NULL, &g_ViewSize, ViewUnmap, 0, PAGE_READWRITE);
    if (!NT_SUCCESS(st)) { Trace("%s: NtMapViewOfSection %08x\n", g_MyName, st); return st; }

    /* P<pid> port. */
    _snwprintf(namebuf, 63, L"\\PSXSS\\PSXSES\\P%u", g_MyPid);
    namebuf[63] = 0;
    RtlInitUnicodeString(&name, namebuf);
    InitializeObjectAttributes(&oa, &name, 0, NULL, NULL);
    st = NtCreatePort(&g_LeaderPort, &oa, 4, 0x70, 0x10000);
    if (!NT_SUCCESS(st)) { Trace("%s: NtCreatePort(P) %08x\n", g_MyName, st); return st; }

    thread = CreateThread(NULL, 0, ServerThread, NULL, 0, NULL);
    if (thread == NULL) return STATUS_UNSUCCESSFUL;
    CloseHandle(thread);
    return STATUS_SUCCESS;
}

/*======================================================================
 * Connect + register at \PSXSS\SESPORT (session leader role).
 *====================================================================*/
static NTSTATUS ConnectSesPort(void)
{
    NTSTATUS st;
    UNICODE_STRING name;
    SECURITY_QUALITY_OF_SERVICE qos;
    ULONG blob = g_MyPid;
    ULONG bloblen = sizeof(blob);

    qos.Length = sizeof(qos);
    qos.ImpersonationLevel = SecurityImpersonation;
    qos.ContextTrackingMode = SECURITY_DYNAMIC_TRACKING;
    qos.EffectiveOnly = TRUE;

    RtlInitUnicodeString(&name, L"\\PSXSS\\SESPORT");
    st = NtConnectPort(&g_SesPort, &name, &qos, NULL, NULL, NULL, &blob, &bloblen);
    if (!NT_SUCCESS(st)) { Trace("%s: NtConnectPort(SESPORT) %08x\n", g_MyName, st); return st; }

    g_SessionId = blob;              /* psxss echoes the assigned session id */
    Trace("%s: registered, session id %u\n", g_MyName, g_SessionId);
    return STATUS_SUCCESS;
}

/*======================================================================
 * Marshal image/cwd/argv/env into D<pid> and ask psxss to spawn the child.
 * Offsets in the argv/env table are relative to ArgvOffset (== the child's
 * CommandLine.Buffer), matching our crt0 (crt/crt0.c: *slot += base).
 *====================================================================*/
static ULONG AlignUp4(ULONG x) { return (x + 3) & ~3u; }

/* TRUE if <drive>:\bin exists (the marker for "the POSIX tree lives here"). */
static BOOL DriveHasPosixTree(char drive)
{
    char probe[8];
    DWORD attrs;
    probe[0] = drive; probe[1] = ':'; probe[2] = '\\';
    probe[3] = 'b';   probe[4] = 'i'; probe[5] = 'n'; probe[6] = 0;
    attrs = GetFileAttributesA(probe);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

/* Pick the drive hosting the POSIX tree (/bin, /etc). psxss derives the POSIX
 * root '/' from the spawn cwd's DRIVE letter, so this choice IS the root: get
 * it wrong and /bin, /etc point at the wrong volume. Probe order: the current
 * directory's drive (installed system, or a LiveCD prompt on X:), then
 * SystemDrive, then every drive on the machine. */
static char PickPosixDrive(void)
{
    char  buf[MAX_PATH];
    DWORD mask;
    int   i;

    if (GetCurrentDirectoryA(sizeof(buf), buf) >= 2 && buf[1] == ':' &&
        DriveHasPosixTree(buf[0]))
        return buf[0];

    if (GetEnvironmentVariableA("SystemDrive", buf, sizeof(buf)) >= 2 &&
        buf[1] == ':' && DriveHasPosixTree(buf[0]))
        return buf[0];

    mask = GetLogicalDrives();
    for (i = 0; i < 26; i++)
        if ((mask & (1u << i)) && DriveHasPosixTree((char)('A' + i)))
            return (char)('A' + i);

    return 'C';     /* nothing found: the historic default, will fail visibly */
}

/* Resolve the typed command to a DOS image path on the POSIX volume:
 *   "name"          -> <root>:\bin\name[.exe]
 *   "/bin/name"     -> <root>:\bin\name[.exe]    (POSIX absolute path)
 *   "dir\x", "D:\x" -> as given [.exe]           (DOS form passes through)
 * .exe is appended only when the path as given doesn't exist -- so both
 * 'nlhello' and 'nlhello.exe' resolve to nlhello.exe (never .exe.exe). */
static void ResolveImage(const char *command, char posixDrive,
                         char *dos, size_t dossz)
{
    size_t len, i;

    if (command[0] == '/')
    {
        _snprintf(dos, dossz - 1, "%c:%s", posixDrive, command);
        dos[dossz - 1] = 0;
        for (i = 0; dos[i]; i++)
            if (dos[i] == '/') dos[i] = '\\';
    }
    else if (strchr(command, '\\') || strchr(command, '/') ||
             (command[0] != 0 && command[1] == ':'))
    {
        _snprintf(dos, dossz - 1, "%s", command);
        dos[dossz - 1] = 0;
    }
    else
    {
        _snprintf(dos, dossz - 1, "%c:\\bin\\%s", posixDrive, command);
        dos[dossz - 1] = 0;
    }

    len = strlen(dos);
    if (GetFileAttributesA(dos) == INVALID_FILE_ATTRIBUTES && len + 4 < dossz &&
        (len < 4 || _stricmp(dos + len - 4, ".exe") != 0))
    {
        strcpy(dos + len, ".exe");
    }
}

/* xargv[0] = the command (bare name or path), xargv[1..] forwarded verbatim to
 * the child (so 'posixterm sh -c "ls /"' works). dosImage receives the resolved
 * image path for error reporting. */
static NTSTATUS SpawnChild(int xargc, char **xargv, char *dosImage, size_t dosImageSize)
{
    char        *sec = (char *)g_SharedBase;
    char         cwd[300];
    WCHAR        wImage[300];
    UNICODE_STRING ntImage;
    ANSI_STRING  aImage;
    ULONG        p, imgOff, cwdOff, argvOff, tableEntries, strArea;
    ULONG       *table;
    SES_MSG      msg;
    NTSTATUS     st;
    char         posixDrive;
    const char  *command;

    /* argv[] for the child: the leader's tail, capped to the table below. */
    const char *argv[64];
    int argc = 0, a;
    /* env[] for the child. TERM=vt100 since we drive John Miller's emulator. */
    static const char *envv[] = {
        "PATH=/bin", "HOME=/", "TERM=vt100", "_POSIX_TERM=on", NULL
    };
    int envc = 0;
    while (envv[envc]) envc++;

    command = (xargc > 0 && xargv[0] != NULL && xargv[0][0] != 0) ? xargv[0] : "sh";
    argv[argc++] = command;
    for (a = 1; a < xargc && argc < (int)(sizeof(argv) / sizeof(argv[0])) - 1; a++)
        argv[argc++] = xargv[a];

    posixDrive = PickPosixDrive();
    ResolveImage(command, posixDrive, dosImage, dosImageSize);

    /* cwd = current dir (DOS form); psxss prepends \DosDevices\ AND derives the
     * POSIX root '/' from this cwd's DRIVE letter (see PickPosixDrive). Keep the
     * real cwd when it lives on the POSIX volume; else use that volume's root. */
    if (GetCurrentDirectoryA(sizeof(cwd) - 2, cwd) == 0 ||
        !(cwd[1] == ':' && (cwd[0] & ~0x20) == (posixDrive & ~0x20)))
    {
        cwd[0] = posixDrive; cwd[1] = ':'; cwd[2] = '\\'; cwd[3] = 0;
    }
    { size_t l = strlen(cwd); if (l && cwd[l - 1] != '\\') { cwd[l] = '\\'; cwd[l + 1] = 0; } }

    /* Image as an NT path (ASCII) at section[0]. */
    MultiByteToWideChar(CP_ACP, 0, dosImage, -1, wImage, 300);
    RtlInitUnicodeString(&ntImage, NULL);
    if (!RtlDosPathNameToNtPathName_U(wImage, &ntImage, NULL, NULL))
        return STATUS_OBJECT_NAME_INVALID;
    RtlUnicodeStringToAnsiString(&aImage, &ntImage, TRUE);

    p = 0;
    imgOff = 0;
    memcpy(sec + p, aImage.Buffer, aImage.Length + 1);
    p = AlignUp4(p + aImage.Length + 1);
    RtlFreeAnsiString(&aImage);
    RtlFreeUnicodeString(&ntImage);

    /* cwd (ASCII). */
    cwdOff = p;
    memcpy(sec + p, cwd, strlen(cwd) + 1);
    p = AlignUp4(p + (ULONG)strlen(cwd) + 1);

    /* argv/env offset table + strings, all relative to argvOff. */
    argvOff = p;
    tableEntries = (ULONG)argc + 1 + (ULONG)envc + 1;   /* argv + NUL + env + NUL */
    table = (ULONG *)(sec + argvOff);
    strArea = argvOff + tableEntries * sizeof(ULONG);
    strArea = AlignUp4(strArea);

    {
        ULONG cur = strArea;
        int i, e = 0;
        for (i = 0; i < argc; i++)
        {
            ULONG l = (ULONG)strlen(argv[i]) + 1;
            memcpy(sec + cur, argv[i], l);
            table[i] = cur - argvOff;                  /* buffer-relative */
            cur += l;
        }
        table[argc] = 0;                                /* argv terminator */
        for (i = 0; envv[i]; i++)
        {
            ULONG l = (ULONG)strlen(envv[i]) + 1;
            memcpy(sec + cur, envv[i], l);
            table[argc + 1 + i] = cur - argvOff;
            cur += l;
            e++;
        }
        table[argc + 1 + e] = 0;                        /* env terminator */
        p = AlignUp4(cur);
    }

    /* Build + send the create-process request over the SESPORT client. */
    RtlZeroMemory(&msg, sizeof(msg));
    msg.Header.u1.s1.TotalLength = 0x50;
    msg.Header.u1.s1.DataLength  = 0x38;
    msg.u[IDX_SELECTOR]  = 0;
    msg.u[IDX_DISCRIM]   = 0;                            /* create process */
    msg.u[IDX_LEADERPID] = g_MyPid;
    msg.u[IDX_SESSIONID] = g_SessionId;
    msg.u[IDX_FDCOUNT]   = 3;                            /* inherit stdin/out/err */
    msg.u[IDX_IMGOFF]    = imgOff;
    msg.u[IDX_CWDOFF]    = cwdOff;
    msg.u[IDX_ARGVOFF]   = argvOff;
    /* Our psxss reads EnvOffset as the env offset-TABLE (for _PSXLIBPATH); it
     * follows the argv table contiguously. */
    msg.u[IDX_ENVOFF]    = argvOff + ((ULONG)argc + 1) * sizeof(ULONG);
    msg.u[IDX_SHAREDBASE] = (ULONG)(ULONG_PTR)g_SharedBase;

    st = NtRequestWaitReplyPort(g_SesPort, &msg.Header, &msg.Header);
    if (!NT_SUCCESS(st)) { Trace("%s: spawn LPC %08x\n", g_MyName, st); return st; }
    if ((LONG)msg.u[IDX_STATUS] < 0) { Trace("%s: spawn reply %08x\n", g_MyName, msg.u[IDX_STATUS]); return (NTSTATUS)msg.u[IDX_STATUS]; }

    g_ReplyContext = msg.u[IDX_SESSIONID];              /* Context @0x2C on reply */
    Trace("%s: spawned '%s' in session %u\n", g_MyName, dosImage, g_SessionId);
    return STATUS_SUCCESS;
}

/*======================================================================
 * Default termios (matches posix.exe InitTty): canonical + echo + signals.
 *====================================================================*/
static void InitTermios(void)
{
    RtlZeroMemory(g_Termios, sizeof(g_Termios));
    T_CIFLAG(g_Termios) = 3;            /* BRKINT|ICRNL */
    T_COFLAG(g_Termios) = 3;            /* OPOST|ONLCR  */
    T_CCFLAG(g_Termios) = 0x82;         /* CREAD|CS8    */
    T_CLFLAG(g_Termios) = 0x57;         /* ECHO|ECHOE|ECHOK|ICANON|ISIG */
    g_Termios[0x18 + 0] = 26;           /* VEOF  = ^Z */
    g_Termios[0x18 + 2] = 8;            /* VERASE= ^H */
    g_Termios[0x18 + 3] = 3;            /* VINTR = ^C */
    g_Termios[0x18 + 4] = 24;           /* VKILL = ^X */
    g_Termios[0x18 + 9] = 17;           /* VSTART= ^Q */
    g_Termios[0x18 + 10] = 19;          /* VSTOP = ^S */
}

/*======================================================================
 * Entry point.
 *====================================================================*/
int main(int argc, char **argv)
{
    NTSTATUS st;
    HANDLE thread;
    char resolvedImage[300];
    static char *defargv[] = { "sh", NULL };
    int    cargc = (argc > 1) ? argc - 1 : 1;
    char **cargv = (argc > 1) ? argv + 1  : defargv;

    g_MyPid = GetCurrentProcessId();

    /* VT100 front-end + console back-end online before any output. */
    vtInitVT100();
    g_hConIn = GetStdHandle(STD_INPUT_HANDLE);
    InitTermios();
    RtlInitializeCriticalSection(&g_InLock);
    RtlInitializeCriticalSection(&g_SesLock);
    g_InReady = CreateEventA(NULL, FALSE, FALSE, NULL);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);   /* Ctrl-C -> SIGINT to the child */

    /* Cold start: if the POSIX subsystem isn't running yet the D<pid> section
     * create fails with OBJECT_PATH_NOT_FOUND. Ask SM to load it and retry. */
    st = CreateSessionObjects();
    if (st == STATUS_OBJECT_PATH_NOT_FOUND)
    {
        int i;
        vtprintf("%s: POSIX subsystem not running -- asking SM to load it...\r\n", g_MyName);
        EnsurePsxSs();
        for (i = 0; i < 40 && !NT_SUCCESS(st); i++)
        {
            Sleep(250);
            st = CreateSessionObjects();
        }
    }
    if (!NT_SUCCESS(st)) { vtprintf("%s: session objects failed (%08x)\r\n", g_MyName, st); return 1; }

    st = ConnectSesPort();
    if (!NT_SUCCESS(st)) { vtprintf("%s: connect to psxss failed (%08x)\r\n", g_MyName, st); return 1; }

    st = SpawnChild(cargc, cargv, resolvedImage, sizeof(resolvedImage));
    if (!NT_SUCCESS(st))
    {
        vtprintf("%s: could not start '%s' -> '%s' (%08x)\r\n",
                 g_MyName, cargv[0], resolvedImage, st);
        return 1;
    }

    /* Drive keyboard input; the P<pid> server thread carries the tty I/O. */
    thread = CreateThread(NULL, 0, InputThread, NULL, 0, NULL);
    if (thread) CloseHandle(thread);

    /* Idle until teardown (ServerThread ExitProcess on the child's exit). */
    for (;;) Sleep(1000);
    return 0;
}
