/*
 * stallmon - find out what stalls ReactOS, rather than merely reproducing it.
 *
 * The symptom under investigation: running an application (Chromium always
 * does it) makes the whole desktop crawl, with the host showing the guest CPU
 * pegged. Pegged means the processor is executing, not halted, so this is a
 * spin or a livelock rather than a classic deadlock - whatever is at fault is
 * *running* when it happens, and can be caught in the act.
 *
 * A program that only reproduces the stall leaves you exactly where you
 * started, so this one measures. Four independent heartbeats run for the
 * whole session and each writes its own line to the debug port. Whichever one
 * goes quiet in serial.log names the subsystem:
 *
 *   CPU quiet, GUI quiet, RT alive    priority starvation. Something is
 *                                     spinning at or above the priority the
 *                                     anti-starvation boost tops out at, and
 *                                     the boost cannot rescue its victims.
 *
 *   CPU alive, GUI quiet              win32k. The scheduler is fine and the
 *                                     GUI is serialised behind something.
 *
 *   CPU quiet, GUI quiet, RT quiet    below the scheduler entirely - raised
 *                                     IRQL, a spinlock, or an interrupt
 *                                     storm. Nothing in user mode can help.
 *
 *   all alive, desktop still dead     the stall is not in this process at
 *                                     all. Look at explorer and csrss.
 *
 * That last row is why the heartbeats are worth having even when you are
 * confident you already know the answer.
 *
 * The load is shaped like the application that provokes it: several
 * processes, each with several windows, each window drawing something
 * different, and continuous cross-process traffic over named pipes. Every
 * ingredient has a switch so a run can be narrowed to one of them - the
 * point is to find which axis matters, not to build a Chromium-shaped blob
 * that stalls without explaining why.
 *
 * Two clocks are logged side by side on purpose. A wrong clock makes a
 * healthy system look stalled, and this tree has already had one stub that
 * returned without writing its output, leaving callers reading their own
 * stack and calling it the time. If the tick and the performance counter
 * disagree about how long something took, distrust the measurement before
 * you distrust the machine.
 *
 * Everything reports through OutputDebugString, so it lands in the host's
 * serial.log and needs neither the console nor a live GUI to be read back.
 * If a heartbeat's own line stops appearing, that is not a broken tool - it
 * is the result.
 *
 * Build (RosBE):
 *     i686-w64-mingw32-gcc -O2 -o stallmon.exe stallmon.c -luser32 -lgdi32
 *
 * See README.md for the experiment matrix.
 */

/* SRWLOCK and CONDITION_VARIABLE are Vista+, and the headers hide them
   below this. base::Lock is an SRWLOCK and base::ConditionVariable is a
   CONDITION_VARIABLE over one, so modelling Chromium needs both. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* Limits                                                              */
/* ------------------------------------------------------------------ */

#define MAX_PROCS           16
#define MAX_WINDOWS         16
#define PIPE_MAGIC          0x4C545321  /* "STL!" */

/* How long a heartbeat may go without a beat before it is called out */
#define STALL_REPORT_MS     1500

/* ------------------------------------------------------------------ */
/* Renderers                                                           */
/* ------------------------------------------------------------------ */

enum
{
    RENDER_GDI = 0,     /* lines and ellipses - the plain GDI path        */
    RENDER_TEXT,        /* DrawText - glyph and font cache                */
    RENDER_BLIT,        /* BitBlt from a memory DC, which is what a       */
                        /* software compositor does to present a frame    */
    RENDER_STRETCH,     /* StretchBlt - scaling path                      */
    RENDER_FILL,        /* solid fills - cheapest possible painting       */
    RENDER_CLIP,        /* region clipping, then fills                    */
    RENDER_COUNT
};

static const char *RenderName(int Mode)
{
    switch (Mode)
    {
        case RENDER_GDI:     return "gdi";
        case RENDER_TEXT:    return "text";
        case RENDER_BLIT:    return "blit";
        case RENDER_STRETCH: return "stretch";
        case RENDER_FILL:    return "fill";
        case RENDER_CLIP:    return "clip";
    }
    return "?";
}

/* ------------------------------------------------------------------ */
/* Cross process message                                               */
/* ------------------------------------------------------------------ */

enum
{
    MSG_PING = 1,       /* parent -> child, echoed straight back        */
    MSG_PONG,           /* child -> parent, carries the ping's sequence */
    MSG_STATUS          /* child -> parent, unsolicited progress report */
};

#pragma pack(push, 1)
typedef struct _STALL_MSG
{
    ULONG Magic;
    ULONG Type;
    ULONG Index;        /* which child                                  */
    ULONG Seq;          /* ping sequence, echoed in the pong            */
    ULONG Tick;         /* GetTickCount at send                         */
    ULONG Frames;       /* frames painted so far, in a status           */
    ULONG PaintMaxMs;   /* worst single WM_PAINT so far, in a status    */
    ULONG GuiGapMaxMs;  /* worst gap between message loop turns         */
} STALL_MSG;
#pragma pack(pop)

/* ------------------------------------------------------------------ */
/* Options                                                             */
/* ------------------------------------------------------------------ */

typedef struct _OPTIONS
{
    BOOL IsChild;
    int Index;              /* child index                              */
    int Procs;              /* how many child processes                 */
    int Windows;            /* windows per child                        */
    int Render;             /* -1 cycles through every renderer         */
    int Fps;                /* invalidations per second per window      */
    int Seconds;            /* run length                               */
    BOOL SpinEnabled;       /* run a spinner at all                     */
    int SpinPriority;       /* which priority - legitimately negative   */
    int IpcMs;              /* ping interval                            */
    BOOL NoGui;             /* no windows at all - the decisive run     */
    BOOL Storm;             /* invalidate as fast as possible           */
    BOOL HiResTimer;        /* ask for a 1ms clock, as Chromium does    */
    BOOL ChromePump;        /* drive the UI the way Chromium's pump does */
    int Pool;               /* thread pool workers per process          */
    int PostHz;             /* cross-thread task posts per second       */
    BOOL SpinYield;         /* spin-then-yield lock contention          */
    BOOL NoYield;           /* CPU heartbeat never calls Sleep(0)       */
    WCHAR PipeName[128];
} OPTIONS;

static OPTIONS gOpt;

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */

static LARGE_INTEGER gQpcFreq;
static DWORD gStartTick;

static void LogInit(void)
{
    if (!QueryPerformanceFrequency(&gQpcFreq))
        gQpcFreq.QuadPart = 0;
    gStartTick = GetTickCount();
}

/* Milliseconds since start, by the performance counter. Reported beside the
   tick so the two can be compared - if they disagree, the measurement is the
   thing that is broken. */
static ULONG QpcMs(void)
{
    LARGE_INTEGER Now;

    if (gQpcFreq.QuadPart == 0 || !QueryPerformanceCounter(&Now))
        return 0;

    return (ULONG)((Now.QuadPart * 1000) / gQpcFreq.QuadPart);
}

static ULONGLONG QpcUs(void)
{
    LARGE_INTEGER Now;

    if (gQpcFreq.QuadPart == 0 || !QueryPerformanceCounter(&Now))
        return 0;

    return (ULONGLONG)((Now.QuadPart * 1000000) / gQpcFreq.QuadPart);
}

static void Log(const char *Format, ...)
{
    char Buffer[512];
    char Line[640];
    va_list Args;

    va_start(Args, Format);
    _vsnprintf(Buffer, sizeof(Buffer) - 1, Format, Args);
    Buffer[sizeof(Buffer) - 1] = '\0';
    va_end(Args);

    _snprintf(Line, sizeof(Line) - 1, "STALL[%s%d] t=%lu q=%lu %s",
              gOpt.IsChild ? "c" : "P", gOpt.Index,
              (unsigned long)(GetTickCount() - gStartTick),
              (unsigned long)QpcMs(),
              Buffer);
    Line[sizeof(Line) - 1] = '\0';

    OutputDebugStringA(Line);
    fputs(Line, stdout);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Timer resolution                                                    */
/* ------------------------------------------------------------------ */

/*
 * The single most system-wide thing Chromium does on Windows.
 * base/time/time_win.cc keeps kMinTimerIntervalHighResMs = 1 and calls
 * timeBeginPeriod(1) whenever it has a high resolution timer pending, which
 * for a running browser is essentially always.
 *
 * That is not a per-process setting. ReactOS honours it: NtSetTimerResolution
 * reaches HalSetTimeIncrement, which reprograms the clock. The default here
 * is HalpLargestClockMS = 15, so the request takes the system from roughly
 * 66 clock interrupts a second to 1000 - fifteen times as many, for every
 * process on the machine, for as long as the browser runs.
 *
 * On real hardware that is affordable. Under emulation each tick is a VM
 * exit, an emulated timer device and an interrupt injection, and the
 * scheduler's whole per-tick burden - quantum accounting, timer expiry,
 * DPC drain - runs fifteen times as often.
 *
 * So query it rather than assume it. Reporting what the resolution actually
 * became, before and after, is what turns "Chromium makes it slow" into a
 * number.
 */

typedef LONG (WINAPI *PFN_NT_QUERY_TIMER_RESOLUTION)(PULONG, PULONG, PULONG);
typedef LONG (WINAPI *PFN_NT_SET_TIMER_RESOLUTION)(ULONG, BOOLEAN, PULONG);

static PFN_NT_QUERY_TIMER_RESOLUTION pNtQueryTimerResolution;
static PFN_NT_SET_TIMER_RESOLUTION pNtSetTimerResolution;

static void TimerResolutionInit(void)
{
    HMODULE Ntdll = GetModuleHandleW(L"ntdll.dll");

    if (Ntdll == NULL)
        return;

    pNtQueryTimerResolution = (PFN_NT_QUERY_TIMER_RESOLUTION)
        GetProcAddress(Ntdll, "NtQueryTimerResolution");
    pNtSetTimerResolution = (PFN_NT_SET_TIMER_RESOLUTION)
        GetProcAddress(Ntdll, "NtSetTimerResolution");
}

/* Resolutions are in 100ns units, so 10000 is a millisecond */
static void ReportTimerResolution(const char *When)
{
    ULONG Minimum = 0, Maximum = 0, Current = 0;

    if (pNtQueryTimerResolution == NULL)
    {
        Log("timer resolution %s: NtQueryTimerResolution not available\n", When);
        return;
    }

    if (pNtQueryTimerResolution(&Minimum, &Maximum, &Current) < 0)
    {
        Log("timer resolution %s: query failed\n", When);
        return;
    }

    /* Minimum is the coarsest the system will go and Maximum the finest,
       which reads backwards until you remember these are periods */
    Log("timer resolution %s: coarsest=%lu.%03lums finest=%lu.%03lums "
        "current=%lu.%03lums (%lu Hz)\n",
        When,
        (unsigned long)(Minimum / 10000), (unsigned long)(Minimum % 10000) / 10,
        (unsigned long)(Maximum / 10000), (unsigned long)(Maximum % 10000) / 10,
        (unsigned long)(Current / 10000), (unsigned long)(Current % 10000) / 10,
        Current ? (unsigned long)(10000000 / Current) : 0);
}

static void RequestHighResolutionTimer(void)
{
    ULONG Current = 0;

    ReportTimerResolution("before");

    if (pNtSetTimerResolution == NULL)
    {
        Log("NtSetTimerResolution not available\n");
        return;
    }

    /* 10000 * 100ns = 1ms, which is exactly what timeBeginPeriod(1) asks for */
    pNtSetTimerResolution(10000, TRUE, &Current);
    ReportTimerResolution("after");
}

/* ------------------------------------------------------------------ */
/* Heartbeats                                                          */
/* ------------------------------------------------------------------ */

typedef struct _HEARTBEAT
{
    const char *Name;
    volatile LONG Beats;
    volatile LONG LastTick;     /* GetTickCount of the last beat        */
    volatile LONG GapMaxMs;     /* worst gap seen between beats         */
    volatile LONG Reported;     /* already called out as stalled        */
} HEARTBEAT;

static HEARTBEAT gCpuBeat = { "CPU" };
static HEARTBEAT gGuiBeat = { "GUI" };
static HEARTBEAT gRtBeat  = { "RT " };
static HEARTBEAT gIpcBeat = { "IPC" };

static volatile LONG gRunning = 1;

static void Beat(HEARTBEAT *Hb)
{
    DWORD Now = GetTickCount();
    LONG Last = Hb->LastTick;
    LONG Gap;

    if (Last != 0)
    {
        Gap = (LONG)(Now - (DWORD)Last);
        if (Gap > Hb->GapMaxMs)
            Hb->GapMaxMs = Gap;
    }

    Hb->LastTick = (LONG)Now;
    InterlockedIncrement(&Hb->Beats);
}

/*
 * Pure computation, no system calls at all. If this one keeps beating while
 * the desktop is dead, the scheduler is still handing this thread time and
 * the fault is somewhere above it.
 */
static DWORD WINAPI CpuBeatThread(LPVOID Param)
{
    volatile ULONG Accumulator = 0;
    ULONG i;

    UNREFERENCED_PARAMETER(Param);

    while (gRunning)
    {
        /* Enough work to be visible, little enough to beat often */
        for (i = 0; i < 200000; i++)
            Accumulator += i ^ (Accumulator >> 3);

        Beat(&gCpuBeat);

        if (!gOpt.NoYield)
            Sleep(0);
    }

    return Accumulator & 1;
}

/*
 * The same loop above the priority at which the anti-starvation boost gives
 * up. If CPU stalls and this does not, the victims are being starved rather
 * than blocked.
 */
static DWORD WINAPI RtBeatThread(LPVOID Param)
{
    volatile ULONG Accumulator = 0;
    ULONG i;

    UNREFERENCED_PARAMETER(Param);

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    while (gRunning)
    {
        for (i = 0; i < 200000; i++)
            Accumulator += i ^ (Accumulator >> 3);

        Beat(&gRtBeat);
        Sleep(0);
    }

    return Accumulator & 1;
}

/*
 * A spinner that never yields, at a priority the caller chooses. This is the
 * suspect, not the instrument: sweep the priority and watch for the point at
 * which the desktop stops coming back.
 */
static DWORD WINAPI SpinThread(LPVOID Param)
{
    volatile ULONG Accumulator = 0;
    int Priority = (int)(INT_PTR)Param;

    SetThreadPriority(GetCurrentThread(), Priority);
    Log("spinner running at thread priority %d\n", Priority);

    while (gRunning)
        Accumulator++;

    return Accumulator & 1;
}

/* ------------------------------------------------------------------ */
/* Watchdog                                                            */
/* ------------------------------------------------------------------ */

static HEARTBEAT *gAllBeats[] = { &gCpuBeat, &gGuiBeat, &gRtBeat, &gIpcBeat };

static void CheckBeat(HEARTBEAT *Hb, DWORD Now)
{
    LONG Last = Hb->LastTick;
    LONG Silent;

    if (Last == 0)
        return;                 /* never started, nothing to say        */

    Silent = (LONG)(Now - (DWORD)Last);

    if (Silent > STALL_REPORT_MS)
    {
        if (!Hb->Reported)
        {
            Hb->Reported = 1;
            Log("*** %s SILENT for %ld ms ***\n", Hb->Name, Silent);
        }
    }
    else if (Hb->Reported)
    {
        Hb->Reported = 0;
        Log("--- %s recovered ---\n", Hb->Name);
    }
}

/*
 * Runs above the heartbeats so it can still speak while they are starved.
 * If even this goes quiet, the stall is below anything user mode can see.
 */
static DWORD WINAPI WatchdogThread(LPVOID Param)
{
    DWORD Next = GetTickCount();
    int i;

    UNREFERENCED_PARAMETER(Param);

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    while (gRunning)
    {
        DWORD Now = GetTickCount();

        for (i = 0; i < (int)(sizeof(gAllBeats) / sizeof(gAllBeats[0])); i++)
            CheckBeat(gAllBeats[i], Now);

        /* A periodic line from every heartbeat, so that a gap in the log is
           itself the evidence - the absence of a line needs no interpreting */
        if ((LONG)(Now - Next) >= 0)
        {
            Next = Now + 2000;
            for (i = 0; i < (int)(sizeof(gAllBeats) / sizeof(gAllBeats[0])); i++)
            {
                HEARTBEAT *Hb = gAllBeats[i];
                if (Hb->LastTick == 0)
                    continue;
                Log("%s beats=%ld gapmax=%ldms\n",
                    Hb->Name, Hb->Beats, Hb->GapMaxMs);
            }
        }

        Sleep(250);
    }

    return 0;
}


/* ------------------------------------------------------------------ */
/* Chromium's UI pump                                                  */
/* ------------------------------------------------------------------ */

/*
 * base/message_loop/message_pump_win.cc, reproduced closely enough to load
 * the same kernel paths. A generic PeekMessage loop does not, and that is
 * why this exists.
 *
 * The pump owns a message-only window. Anything wanting the UI thread to run
 * a task does not touch a queue the UI thread polls - it calls
 *
 *     ::PostMessage(message_window_.hwnd(), kMsgHaveWork, 0, 0)
 *
 * so every cross-thread task post is a win32k call, and on the receiving side
 * a message dispatch. A busy browser does this thousands of times a second
 * from its IO and pool threads. The native_msg_scheduled_ flag exists to stop
 * more than one being in flight at once, and is modelled here because without
 * it the queue behaviour is completely different.
 *
 * When idle the pump does not spin - it blocks in
 *
 *     MsgWaitForMultipleObjectsEx(1, &event, delay, QS_ALLINPUT,
 *                                 MWMO_INPUTAVAILABLE)
 *
 * waiting on an event and the message queue together. That single call is the
 * heart of the loop and exercises a path a PeekMessage loop never reaches.
 *
 * Delayed work is a ::SetTimer on the same window, killed and reset whenever
 * the next delayed time moves - the source calls this out as
 * "unnecessary ::SetTimer<=>::KillTimer churn", and the churn is real win32k
 * traffic, so it is reproduced too.
 */

#define kMsgHaveWork (WM_USER + 1)

typedef struct _CHROME_PUMP
{
    HWND MessageWindow;
    HANDLE Event;                   /* the pump's WaitableEvent          */
    volatile LONG NativeMsgScheduled;
    volatile LONG WorkCount;
    volatile LONG TimerChurn;
    volatile LONG PostFailures;
} CHROME_PUMP;

static CHROME_PUMP gPump;

static LRESULT CALLBACK PumpWndProc(HWND Wnd, UINT Msg, WPARAM wParam,
                                    LPARAM lParam)
{
    if (Msg == kMsgHaveWork)
    {
        /* Chromium clears the flag here so a replacement post can be made,
           then does its work. The order matters: clearing late would let the
           queue drain to empty and stall the loop. */
        InterlockedExchange(&gPump.NativeMsgScheduled, 0);
        InterlockedIncrement(&gPump.WorkCount);
        return 0;
    }

    if (Msg == WM_TIMER)
        return 0;

    return DefWindowProcW(Wnd, Msg, wParam, lParam);
}

static BOOL PumpCreate(void)
{
    WNDCLASSEXW Class;

    ZeroMemory(&Class, sizeof(Class));
    Class.cbSize = sizeof(Class);
    Class.lpfnWndProc = PumpWndProc;
    Class.hInstance = GetModuleHandleW(NULL);
    Class.lpszClassName = L"StallMonPumpWindow";
    RegisterClassExW(&Class);

    /* HWND_MESSAGE: no pixels, never visible, exists only to receive posts */
    gPump.MessageWindow = CreateWindowExW(0, L"StallMonPumpWindow",
                                          L"stallmon pump",
                                          0, 0, 0, 0, 0,
                                          HWND_MESSAGE, NULL,
                                          GetModuleHandleW(NULL), NULL);
    if (gPump.MessageWindow == NULL)
    {
        Log("message-only window failed, error %lu\n", GetLastError());
        return FALSE;
    }

    gPump.Event = CreateEventW(NULL, FALSE, FALSE, NULL);
    return gPump.Event != NULL;
}

/* What a pool or IO thread calls to give the UI thread work to do */
static void PumpScheduleWork(void)
{
    LONG Was = InterlockedCompareExchange(&gPump.NativeMsgScheduled, 1, 0);

    if (Was != 0)
        return;                     /* one is already in flight            */

    if (!PostMessageW(gPump.MessageWindow, kMsgHaveWork, 0, 0))
    {
        /* The queue is full - about 2000 messages. Chromium treats this as
           recoverable and so do we, but it is worth counting: a queue that
           fills up is a UI thread that is not draining it. */
        InterlockedExchange(&gPump.NativeMsgScheduled, 0);
        InterlockedIncrement(&gPump.PostFailures);
    }
}

/*
 * One turn of the pump. Returns FALSE when it is time to stop.
 */
static BOOL PumpRunOnce(DWORD IdleTimeoutMs)
{
    MSG Msg;
    BOOL DidWork = FALSE;

    while (PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE))
    {
        if (Msg.message == WM_QUIT)
            return FALSE;

        TranslateMessage(&Msg);
        DispatchMessageW(&Msg);
        DidWork = TRUE;
    }

    Beat(&gGuiBeat);

    if (DidWork)
        return TRUE;

    /* Idle. Block on the event and the message queue together, which is the
       call Chromium actually sits in and a plain message loop never makes. */
    MsgWaitForMultipleObjectsEx(1, &gPump.Event, IdleTimeoutMs,
                                QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    return TRUE;
}

/* The SetTimer/KillTimer churn Chromium generates for delayed work */
static void PumpChurnTimer(void)
{
    static UINT_PTR TimerId = 1;

    KillTimer(gPump.MessageWindow, TimerId);
    SetTimer(gPump.MessageWindow, TimerId, 10, NULL);
    InterlockedIncrement(&gPump.TimerChurn);
}

/* ------------------------------------------------------------------ */
/* Thread pool                                                         */
/* ------------------------------------------------------------------ */

/*
 * Chromium's pool threads sleep on a WaitableEvent and are woken to run a
 * task, which then posts its result back to the UI thread. Reproduced here
 * because the wakeup path - event signal, scheduler dispatch, then a
 * PostMessage into win32k - is the one a browser walks constantly, and the
 * combination is what makes it interesting: it crosses the synchronisation
 * layer and the window manager on every single task.
 */

typedef struct _POOL_WORKER
{
    HANDLE Event;
    HANDLE Thread;
    volatile LONG Tasks;
    int Index;
} POOL_WORKER;

static POOL_WORKER gPool[32];
static int gPoolCount;

/* A lock two workers fight over, so the pool is not embarrassingly parallel.
   base::Lock is an SRWLOCK, and its condition variable partner is what a
   lost wakeup would strand. */
static SRWLOCK gPoolLock;
static CONDITION_VARIABLE gPoolCondition;
static volatile LONG gSharedCounter;

/*
 * A spin-then-yield lock, the shape Chromium's low level spinlocks use:
 * spin a while, then SwitchToThread, then Sleep(0), then Sleep(1). On a
 * uniprocessor this is the classic way to peg a CPU without making progress,
 * because the spinner can be rescheduled ahead of the holder it is waiting
 * for. Off by default - it is a suspect, not scenery.
 */
static volatile LONG gSpinLock;

static void SpinLockAcquire(void)
{
    int Spins = 0;

    while (InterlockedCompareExchange(&gSpinLock, 1, 0) != 0)
    {
        if (++Spins < 100)
        {
            YieldProcessor();
        }
        else if (Spins < 200)
        {
            SwitchToThread();
        }
        else if (Spins < 300)
        {
            Sleep(0);
        }
        else
        {
            Sleep(1);
        }
    }
}

static void SpinLockRelease(void)
{
    InterlockedExchange(&gSpinLock, 0);
}

static DWORD WINAPI PoolWorkerThread(LPVOID Param)
{
    POOL_WORKER *Worker = (POOL_WORKER *)Param;

    while (gRunning)
    {
        if (WaitForSingleObject(Worker->Event, 100) == WAIT_OBJECT_0)
        {
            volatile ULONG Work = 0;
            ULONG i;

            /* A little real work, under a lock, the way a task that touches
               shared state would */
            AcquireSRWLockExclusive(&gPoolLock);
            for (i = 0; i < 2000; i++)
                Work += i;
            gSharedCounter++;
            ReleaseSRWLockExclusive(&gPoolLock);
            WakeConditionVariable(&gPoolCondition);

            if (gOpt.SpinYield)
            {
                SpinLockAcquire();
                for (i = 0; i < 500; i++)
                    Work += i;
                SpinLockRelease();
            }

            InterlockedIncrement(&Worker->Tasks);

            /* And hand the result back to the UI thread, which is where the
               PostMessage traffic comes from */
            if (gOpt.ChromePump && gPump.MessageWindow != NULL)
                PumpScheduleWork();
        }
    }

    return 0;
}

/* Wakes the pool at the requested rate, as an IO thread delivering work would */
static DWORD WINAPI PoolPosterThread(LPVOID Param)
{
    int Next = 0;
    DWORD IntervalMs;

    UNREFERENCED_PARAMETER(Param);

    IntervalMs = (gOpt.PostHz > 0) ? (1000 / gOpt.PostHz) : 10;
    if (IntervalMs == 0)
        IntervalMs = 1;

    while (gRunning)
    {
        if (gPoolCount > 0)
        {
            SetEvent(gPool[Next % gPoolCount].Event);
            Next++;
        }

        if (gOpt.ChromePump && gPump.MessageWindow != NULL)
        {
            PumpScheduleWork();
            /* Delayed work moves constantly in a browser, so the timer is
               reset constantly too */
            if ((Next & 15) == 0)
                PumpChurnTimer();
        }

        Sleep(IntervalMs);
    }

    return 0;
}

static void PoolStart(int Count)
{
    int i;

    InitializeSRWLock(&gPoolLock);
    InitializeConditionVariable(&gPoolCondition);

    if (Count > (int)(sizeof(gPool) / sizeof(gPool[0])))
        Count = (int)(sizeof(gPool) / sizeof(gPool[0]));

    for (i = 0; i < Count; i++)
    {
        gPool[i].Index = i;
        gPool[i].Event = CreateEventW(NULL, FALSE, FALSE, NULL);
        gPool[i].Thread = CreateThread(NULL, 0, PoolWorkerThread,
                                       &gPool[i], 0, NULL);

        /* Chromium spreads its threads across the priority tiers rather than
           leaving them all at normal - see platform_thread_win.cc */
        if ((i % 4) == 0)
            SetThreadPriority(gPool[i].Thread, THREAD_PRIORITY_BELOW_NORMAL);
        else if ((i % 4) == 3)
            SetThreadPriority(gPool[i].Thread, THREAD_PRIORITY_ABOVE_NORMAL);
    }

    gPoolCount = Count;
    Log("thread pool: %d workers, posting at %d Hz, spinyield=%d\n",
        Count, gOpt.PostHz, gOpt.SpinYield);
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

typedef struct _WINDOW_STATE
{
    int Render;
    ULONG Frame;
    ULONG PaintMaxMs;
    HWND Wnd;
    HDC MemDc;              /* for the blit renderers                   */
    HBITMAP MemBitmap;
    int MemW, MemH;
} WINDOW_STATE;

static void EnsureMemDc(WINDOW_STATE *State, HDC Dc, int Width, int Height)
{
    if (State->MemDc != NULL && State->MemW == Width && State->MemH == Height)
        return;

    if (State->MemDc != NULL)
    {
        DeleteObject(State->MemBitmap);
        DeleteDC(State->MemDc);
    }

    State->MemDc = CreateCompatibleDC(Dc);
    State->MemBitmap = CreateCompatibleBitmap(Dc, Width, Height);
    SelectObject(State->MemDc, State->MemBitmap);
    State->MemW = Width;
    State->MemH = Height;
}

static void RenderFrame(WINDOW_STATE *State, HDC Dc, RECT *Client)
{
    int Width = Client->right - Client->left;
    int Height = Client->bottom - Client->top;
    ULONG Frame = State->Frame;
    HBRUSH Brush;
    RECT Rect;
    int i;

    if (Width <= 0 || Height <= 0)
        return;

    switch (State->Render)
    {
        case RENDER_GDI:
        {
            HPEN Pen = CreatePen(PS_SOLID, 1, RGB(Frame & 0xFF, 0x40, 0xC0));
            HGDIOBJ Old = SelectObject(Dc, Pen);

            for (i = 0; i < 24; i++)
            {
                int x = ((Frame + i * 7) % (Width ? Width : 1));
                MoveToEx(Dc, x, 0, NULL);
                LineTo(Dc, Width - x, Height);
            }
            Ellipse(Dc,
                    (int)(Frame % 32), (int)(Frame % 16),
                    Width - (int)(Frame % 32), Height - (int)(Frame % 16));

            SelectObject(Dc, Old);
            DeleteObject(Pen);
            break;
        }

        case RENDER_TEXT:
        {
            char Text[128];
            SetBkMode(Dc, TRANSPARENT);
            SetTextColor(Dc, RGB(0x20, Frame & 0xFF, 0x20));
            for (i = 0; i < 8; i++)
            {
                _snprintf(Text, sizeof(Text) - 1,
                          "frame %lu line %d the quick brown fox",
                          (unsigned long)Frame, i);
                Text[sizeof(Text) - 1] = '\0';
                Rect = *Client;
                Rect.top += i * 14;
                DrawTextA(Dc, Text, -1, &Rect, DT_LEFT | DT_SINGLELINE);
            }
            break;
        }

        case RENDER_BLIT:
        {
            /* Draw into a memory DC and present it in one blit. This is the
               shape of a software compositor's frame, which is the path a
               browser falls back to when there is no working GPU. */
            EnsureMemDc(State, Dc, Width, Height);
            Rect.left = 0; Rect.top = 0;
            Rect.right = Width; Rect.bottom = Height;
            Brush = CreateSolidBrush(RGB(0x10, 0x10, (Frame * 4) & 0xFF));
            FillRect(State->MemDc, &Rect, Brush);
            DeleteObject(Brush);

            Brush = CreateSolidBrush(RGB(0xE0, (Frame * 8) & 0xFF, 0x30));
            Rect.left = (int)(Frame % 40);
            Rect.top = (int)(Frame % 20);
            Rect.right = Rect.left + Width / 2;
            Rect.bottom = Rect.top + Height / 2;
            FillRect(State->MemDc, &Rect, Brush);
            DeleteObject(Brush);

            BitBlt(Dc, 0, 0, Width, Height, State->MemDc, 0, 0, SRCCOPY);
            break;
        }

        case RENDER_STRETCH:
        {
            EnsureMemDc(State, Dc, 64, 64);
            Rect.left = 0; Rect.top = 0; Rect.right = 64; Rect.bottom = 64;
            Brush = CreateSolidBrush(RGB((Frame * 6) & 0xFF, 0x80, 0x40));
            FillRect(State->MemDc, &Rect, Brush);
            DeleteObject(Brush);

            StretchBlt(Dc, 0, 0, Width, Height,
                       State->MemDc, 0, 0, 64, 64, SRCCOPY);
            break;
        }

        case RENDER_FILL:
        {
            Brush = CreateSolidBrush(RGB((Frame * 3) & 0xFF,
                                         (Frame * 5) & 0xFF,
                                         (Frame * 7) & 0xFF));
            FillRect(Dc, Client, Brush);
            DeleteObject(Brush);
            break;
        }

        case RENDER_CLIP:
        {
            HRGN Region = CreateEllipticRgn((int)(Frame % 20),
                                            (int)(Frame % 10),
                                            Width - (int)(Frame % 20),
                                            Height - (int)(Frame % 10));
            SelectClipRgn(Dc, Region);
            Brush = CreateSolidBrush(RGB(0x40, (Frame * 9) & 0xFF, 0xA0));
            FillRect(Dc, Client, Brush);
            DeleteObject(Brush);
            SelectClipRgn(Dc, NULL);
            DeleteObject(Region);
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Child: windows                                                      */
/* ------------------------------------------------------------------ */

static WINDOW_STATE gWindows[MAX_WINDOWS];
static volatile LONG gTotalFrames;
static volatile LONG gPaintMaxMs;

static LRESULT CALLBACK WndProc(HWND Wnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    WINDOW_STATE *State = (WINDOW_STATE *)GetWindowLongPtrW(Wnd, GWLP_USERDATA);

    switch (Msg)
    {
        case WM_PAINT:
        {
            PAINTSTRUCT Ps;
            RECT Client;
            DWORD Before, Elapsed;
            HDC Dc;

            Before = GetTickCount();
            Dc = BeginPaint(Wnd, &Ps);
            GetClientRect(Wnd, &Client);

            if (State != NULL)
            {
                State->Frame++;
                RenderFrame(State, Dc, &Client);
            }

            EndPaint(Wnd, &Ps);
            Elapsed = GetTickCount() - Before;

            if (State != NULL && Elapsed > State->PaintMaxMs)
                State->PaintMaxMs = Elapsed;
            if ((LONG)Elapsed > gPaintMaxMs)
                gPaintMaxMs = (LONG)Elapsed;

            InterlockedIncrement(&gTotalFrames);
            return 0;
        }

        case WM_ERASEBKGND:
            /* The renderer covers the whole client area, and erasing first
               would double the work and hide what painting really costs */
            return 1;

        case WM_TIMER:
            InvalidateRect(Wnd, NULL, FALSE);
            return 0;

        case WM_CLOSE:
            gRunning = 0;
            return 0;

        case WM_DESTROY:
            return 0;
    }

    return DefWindowProcW(Wnd, Msg, wParam, lParam);
}

static BOOL RegisterWindowClass(void)
{
    WNDCLASSEXW Class;

    ZeroMemory(&Class, sizeof(Class));
    Class.cbSize = sizeof(Class);
    Class.style = CS_DBLCLKS;
    Class.lpfnWndProc = WndProc;
    Class.hInstance = GetModuleHandleW(NULL);
    Class.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)); /* IDC_ARROW */
    Class.hbrBackground = NULL;
    Class.lpszClassName = L"StallMonWindow";

    if (!RegisterClassExW(&Class))
    {
        Log("RegisterClassExW failed, error %lu\n", GetLastError());
        return FALSE;
    }

    return TRUE;
}

static BOOL CreateWindows(int Count, int RenderBase)
{
    int ScreenW = GetSystemMetrics(SM_CXSCREEN);
    int ScreenH = GetSystemMetrics(SM_CYSCREEN);
    int Cols = 4;
    int CellW = ScreenW / Cols;
    int CellH = ScreenH / 4;
    int i;

    for (i = 0; i < Count; i++)
    {
        WCHAR Title[128];
        HWND Wnd;
        int Slot = (gOpt.Index * MAX_WINDOWS + i);
        int x = (Slot % Cols) * CellW;
        int y = ((Slot / Cols) % 4) * CellH;

        gWindows[i].Render = (RenderBase < 0)
                             ? ((gOpt.Index + i) % RENDER_COUNT)
                             : (RenderBase % RENDER_COUNT);

        wsprintfW(Title, L"stallmon %d.%d %S", gOpt.Index, i,
                  RenderName(gWindows[i].Render));

        Wnd = CreateWindowExW(0,
                              L"StallMonWindow",
                              Title,
                              WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              x, y,
                              CellW - 4, CellH - 4,
                              NULL, NULL,
                              GetModuleHandleW(NULL),
                              NULL);
        if (Wnd == NULL)
        {
            Log("CreateWindowExW %d failed, error %lu\n", i, GetLastError());
            return FALSE;
        }

        gWindows[i].Wnd = Wnd;
        SetWindowLongPtrW(Wnd, GWLP_USERDATA, (LONG_PTR)&gWindows[i]);

        if (!gOpt.Storm)
        {
            UINT Period = (gOpt.Fps > 0) ? (1000 / gOpt.Fps) : 33;
            if (Period == 0)
                Period = 1;
            SetTimer(Wnd, 1, Period, NULL);
        }

        Log("window %d '%S' renderer %s\n", i, Title,
            RenderName(gWindows[i].Render));
    }

    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Child: pipe                                                         */
/* ------------------------------------------------------------------ */

static HANDLE gChildPipe = INVALID_HANDLE_VALUE;

/*
 * Answers pings and reports progress. Kept on its own thread so that a
 * wedged message loop does not silence the reports, and so the process has
 * the same shape as the applications that provoke this - a UI thread and an
 * IO thread that must hand work to each other.
 */
static DWORD WINAPI ChildPipeThread(LPVOID Param)
{
    DWORD LastStatus = 0;

    UNREFERENCED_PARAMETER(Param);

    while (gRunning)
    {
        STALL_MSG In, Out;
        DWORD Read = 0, Written = 0;
        DWORD Now;

        if (!ReadFile(gChildPipe, &In, sizeof(In), &Read, NULL) ||
            Read != sizeof(In))
        {
            Log("pipe read failed, error %lu\n", GetLastError());
            break;
        }

        if (In.Magic != PIPE_MAGIC)
            continue;

        if (In.Type == MSG_PING)
        {
            ZeroMemory(&Out, sizeof(Out));
            Out.Magic = PIPE_MAGIC;
            Out.Type = MSG_PONG;
            Out.Index = (ULONG)gOpt.Index;
            Out.Seq = In.Seq;
            Out.Tick = In.Tick;

            if (!WriteFile(gChildPipe, &Out, sizeof(Out), &Written, NULL))
            {
                Log("pipe write failed, error %lu\n", GetLastError());
                break;
            }
        }

        Now = GetTickCount();
        if (Now - LastStatus >= 1000)
        {
            LastStatus = Now;
            ZeroMemory(&Out, sizeof(Out));
            Out.Magic = PIPE_MAGIC;
            Out.Type = MSG_STATUS;
            Out.Index = (ULONG)gOpt.Index;
            Out.Tick = Now;
            Out.Frames = (ULONG)gTotalFrames;
            Out.PaintMaxMs = (ULONG)gPaintMaxMs;
            Out.GuiGapMaxMs = (ULONG)gGuiBeat.GapMaxMs;
            WriteFile(gChildPipe, &Out, sizeof(Out), &Written, NULL);
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Child: main                                                         */
/* ------------------------------------------------------------------ */

static int ChildMain(void)
{
    HANDLE Threads[4];
    int ThreadCount = 0;
    DWORD Deadline;
    MSG Msg;

    Log("child starting: windows=%d render=%d fps=%d nogui=%d storm=%d\n",
        gOpt.Windows, gOpt.Render, gOpt.Fps, gOpt.NoGui, gOpt.Storm);

    gChildPipe = CreateFileW(gOpt.PipeName, GENERIC_READ | GENERIC_WRITE,
                             0, NULL, OPEN_EXISTING, 0, NULL);
    if (gChildPipe == INVALID_HANDLE_VALUE)
    {
        Log("cannot open pipe %S, error %lu\n", gOpt.PipeName, GetLastError());
        return 1;
    }

    Threads[ThreadCount++] = CreateThread(NULL, 0, ChildPipeThread, NULL, 0, NULL);
    Threads[ThreadCount++] = CreateThread(NULL, 0, CpuBeatThread, NULL, 0, NULL);

    if (gOpt.ChromePump && !gOpt.NoGui)
    {
        if (!PumpCreate())
            return 1;
    }

    if (gOpt.Pool > 0)
    {
        PoolStart(gOpt.Pool);
        Threads[ThreadCount++] = CreateThread(NULL, 0, PoolPosterThread,
                                              NULL, 0, NULL);
    }

    if (gOpt.SpinEnabled)
    {
        Threads[ThreadCount++] = CreateThread(NULL, 0, SpinThread,
                                              (LPVOID)(INT_PTR)gOpt.SpinPriority,
                                              0, NULL);
    }

    Deadline = GetTickCount() + (DWORD)gOpt.Seconds * 1000;

    if (gOpt.NoGui)
    {
        /* No windows at all. If the desktop still stalls in this mode, the
           GUI is a victim rather than the cause, and that is the single most
           useful thing this program can tell you. */
        while (gRunning && (LONG)(GetTickCount() - Deadline) < 0)
            Sleep(50);
    }
    else
    {
        if (!RegisterWindowClass())
            return 1;
        if (!CreateWindows(gOpt.Windows, gOpt.Render))
            return 1;

        while (gRunning && (LONG)(GetTickCount() - Deadline) < 0)
        {
            if (gOpt.ChromePump)
            {
                /* The real pump: blocks in MsgWaitForMultipleObjectsEx when
                   idle rather than polling, and is woken by kMsgHaveWork */
                if (!PumpRunOnce(gOpt.Storm ? 0 : 10))
                    break;

                if (gOpt.Storm)
                {
                    int w;
                    for (w = 0; w < gOpt.Windows; w++)
                    {
                        if (gWindows[w].Wnd != NULL)
                            InvalidateRect(gWindows[w].Wnd, NULL, FALSE);
                    }
                }
                continue;
            }

            /* PeekMessage rather than GetMessage: the loop has to keep
               beating even with no messages waiting, otherwise a quiet
               period is indistinguishable from a stalled one */
            while (PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&Msg);
                DispatchMessageW(&Msg);
            }

            Beat(&gGuiBeat);

            if (gOpt.Storm)
            {
                int w;
                for (w = 0; w < gOpt.Windows; w++)
                {
                    if (gWindows[w].Wnd != NULL)
                        InvalidateRect(gWindows[w].Wnd, NULL, FALSE);
                }
            }
            else
            {
                Sleep(1);
            }
        }
    }

    gRunning = 0;
    Log("child finished: frames=%ld paintmax=%ldms guigap=%ldms cpugap=%ldms\n",
        gTotalFrames, gPaintMaxMs, gGuiBeat.GapMaxMs, gCpuBeat.GapMaxMs);

    if (gOpt.ChromePump)
    {
        Log("pump: havework=%ld timerchurn=%ld postfailures=%ld\n",
            gPump.WorkCount, gPump.TimerChurn, gPump.PostFailures);
    }
    if (gPoolCount > 0)
    {
        int w;
        LONG Total = 0;
        for (w = 0; w < gPoolCount; w++)
            Total += gPool[w].Tasks;
        Log("pool: %d workers ran %ld tasks, shared counter %ld\n",
            gPoolCount, Total, gSharedCounter);
    }

    WaitForMultipleObjects(ThreadCount, Threads, TRUE, 3000);
    CloseHandle(gChildPipe);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Parent                                                              */
/* ------------------------------------------------------------------ */

typedef struct _CHILD
{
    HANDLE Process;
    HANDLE Pipe;
    HANDLE Thread;
    int Index;
    volatile LONG Frames;
    volatile LONG PaintMaxMs;
    volatile LONG RttMaxUs;
    volatile LONG RttLastUs;
    volatile LONG Pongs;
} CHILD;

static CHILD gChildren[MAX_PROCS];

/*
 * One of these per child: pings, waits for the echo, and folds in whatever
 * status arrives on the way. The round trip is the cross-process latency
 * heartbeat - it covers the pipe, the scheduler at both ends, and the two
 * message loops, so when it stalls alone the problem is between processes
 * rather than inside one.
 */
static DWORD WINAPI ParentChildThread(LPVOID Param)
{
    CHILD *Child = (CHILD *)Param;
    ULONG Seq = 0;

    if (!ConnectNamedPipe(Child->Pipe, NULL) &&
        GetLastError() != ERROR_PIPE_CONNECTED)
    {
        Log("child %d never connected, error %lu\n",
            Child->Index, GetLastError());
        return 1;
    }

    Log("child %d connected\n", Child->Index);

    while (gRunning)
    {
        STALL_MSG Out, In;
        DWORD Written = 0, Read = 0;
        DWORD SentAt;
        ULONGLONG SentAtUs;

        ZeroMemory(&Out, sizeof(Out));
        Out.Magic = PIPE_MAGIC;
        Out.Type = MSG_PING;
        Out.Index = (ULONG)Child->Index;
        Out.Seq = ++Seq;
        SentAt = GetTickCount();
        SentAtUs = QpcUs();
        Out.Tick = SentAt;

        if (!WriteFile(Child->Pipe, &Out, sizeof(Out), &Written, NULL))
            break;

        /* Read until the echo comes back, folding in any status seen on the
           way. A pong that never arrives shows up as the IPC heartbeat going
           quiet, which is exactly what should happen. */
        for (;;)
        {
            if (!ReadFile(Child->Pipe, &In, sizeof(In), &Read, NULL) ||
                Read != sizeof(In))
            {
                goto Done;
            }

            if (In.Magic != PIPE_MAGIC)
                continue;

            if (In.Type == MSG_STATUS)
            {
                Child->Frames = (LONG)In.Frames;
                Child->PaintMaxMs = (LONG)In.PaintMaxMs;
                continue;
            }

            if (In.Type == MSG_PONG && In.Seq == Seq)
            {
                LONG Rtt = (LONG)(QpcUs() - SentAtUs);
                Child->RttLastUs = Rtt;
                if (Rtt > Child->RttMaxUs)
                    Child->RttMaxUs = Rtt;
                InterlockedIncrement(&Child->Pongs);
                Beat(&gIpcBeat);
                break;
            }
        }

        Sleep(gOpt.IpcMs);
    }

Done:
    Log("child %d pipe thread ending\n", Child->Index);
    return 0;
}

static BOOL SpawnChild(int Index)
{
    WCHAR Exe[MAX_PATH];
    WCHAR Command[512];
    WCHAR PipeName[128];
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;
    CHILD *Child = &gChildren[Index];

    Child->Index = Index;

    wsprintfW(PipeName,
              L"\\\\.\\pipe\\stallmon_%lu_%d",
              (unsigned long)GetCurrentProcessId(), Index);

    Child->Pipe = CreateNamedPipeW(PipeName,
                                   PIPE_ACCESS_DUPLEX,
                                   PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE |
                                       PIPE_WAIT,
                                   MAX_PROCS,
                                   sizeof(STALL_MSG) * 4,
                                   sizeof(STALL_MSG) * 4,
                                   0,
                                   NULL);
    if (Child->Pipe == INVALID_HANDLE_VALUE)
    {
        Log("CreateNamedPipeW for child %d failed, error %lu\n",
            Index, GetLastError());
        return FALSE;
    }

    if (!GetModuleFileNameW(NULL, Exe, MAX_PATH))
        return FALSE;

    {
        wsprintfW(Command,
                  L"\"%s\" --child --index %d --pipe %s --windows %d "
                  L"--render %d --fps %d --seconds %d --ipc %d%s%s",
                  Exe, Index, PipeName, gOpt.Windows, gOpt.Render,
                  gOpt.Fps, gOpt.Seconds, gOpt.IpcMs,
                  gOpt.NoGui ? L" --nogui" : L"",
                  gOpt.Storm ? L" --storm" : L"");

        if (gOpt.SpinEnabled)
        {
            WCHAR Spin[64];
            wsprintfW(Spin, L" --spin %d", gOpt.SpinPriority);
            lstrcatW(Command, Spin);
        }
        if (gOpt.ChromePump)
            lstrcatW(Command, L" --chromepump");
        if (gOpt.SpinYield)
            lstrcatW(Command, L" --spinyield");
        if (gOpt.NoYield)
            lstrcatW(Command, L" --noyield");
        if (gOpt.Pool > 0)
        {
            WCHAR Pool[64];
            wsprintfW(Pool, L" --pool %d --posthz %d", gOpt.Pool, gOpt.PostHz);
            lstrcatW(Command, Pool);
        }
    }

    ZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);

    if (!CreateProcessW(NULL, Command, NULL, NULL, FALSE,
                        0, NULL, NULL, &StartupInfo, &ProcessInfo))
    {
        Log("CreateProcessW for child %d failed, error %lu\n",
            Index, GetLastError());
        return FALSE;
    }

    CloseHandle(ProcessInfo.hThread);
    Child->Process = ProcessInfo.hProcess;

    Child->Thread = CreateThread(NULL, 0, ParentChildThread, Child, 0, NULL);
    return TRUE;
}

static int ParentMain(void)
{
    HANDLE Beats[3];
    int BeatCount = 0;
    HANDLE Watchdog;
    DWORD Deadline;
    int i;

    Log("=== stallmon ===\n");

    /*
     * Do this before anything else and report it, because it changes the
     * machine rather than this process, and every other number in the run
     * has to be read in the light of it.
     */
    if (gOpt.HiResTimer)
        RequestHighResolutionTimer();
    else
        ReportTimerResolution("at start");
    Log("procs=%d windows=%d render=%d fps=%d storm=%d nogui=%d "
        "ipc=%dms seconds=%d\n",
        gOpt.Procs, gOpt.Windows, gOpt.Render, gOpt.Fps, gOpt.Storm,
        gOpt.NoGui, gOpt.IpcMs, gOpt.Seconds);
    if (gOpt.SpinEnabled)
        Log("spinner enabled at thread priority %d\n", gOpt.SpinPriority);
    {
        SYSTEM_INFO SystemInfo;
        GetSystemInfo(&SystemInfo);
        Log("processors=%lu qpcfreq=%lu\n",
            (unsigned long)SystemInfo.dwNumberOfProcessors,
            (unsigned long)gQpcFreq.LowPart);
    }

    Beats[BeatCount++] = CreateThread(NULL, 0, CpuBeatThread, NULL, 0, NULL);
    Beats[BeatCount++] = CreateThread(NULL, 0, RtBeatThread, NULL, 0, NULL);

    if (gOpt.Pool > 0)
    {
        PoolStart(gOpt.Pool);
        CreateThread(NULL, 0, PoolPosterThread, NULL, 0, NULL);
    }

    if (gOpt.SpinEnabled)
    {
        Beats[BeatCount++] = CreateThread(NULL, 0, SpinThread,
                                          (LPVOID)(INT_PTR)gOpt.SpinPriority,
                                          0, NULL);
    }

    Watchdog = CreateThread(NULL, 0, WatchdogThread, NULL, 0, NULL);

    for (i = 0; i < gOpt.Procs; i++)
    {
        if (!SpawnChild(i))
            break;
    }

    /* The parent runs a message loop of its own so that its GUI heartbeat
       measures the same desktop the children are competing for */
    if (!gOpt.NoGui)
    {
        MSG Msg;
        Deadline = GetTickCount() + (DWORD)gOpt.Seconds * 1000;
        while (gRunning && (LONG)(GetTickCount() - Deadline) < 0)
        {
            while (PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&Msg);
                DispatchMessageW(&Msg);
            }
            Beat(&gGuiBeat);
            Sleep(5);
        }
    }
    else
    {
        Deadline = GetTickCount() + (DWORD)gOpt.Seconds * 1000;
        while (gRunning && (LONG)(GetTickCount() - Deadline) < 0)
            Sleep(50);
    }

    gRunning = 0;

    ReportTimerResolution("at end");
    Log("=== summary ===\n");
    for (i = 0; i < (int)(sizeof(gAllBeats) / sizeof(gAllBeats[0])); i++)
    {
        HEARTBEAT *Hb = gAllBeats[i];
        Log("%s beats=%ld gapmax=%ldms%s\n",
            Hb->Name, Hb->Beats, Hb->GapMaxMs,
            Hb->LastTick == 0 ? "  (never ran)" : "");
    }

    for (i = 0; i < gOpt.Procs; i++)
    {
        CHILD *Child = &gChildren[i];
        Log("child %d frames=%ld paintmax=%ldms rttmax=%ldus rttlast=%ldus "
            "pongs=%ld\n",
            i, Child->Frames, Child->PaintMaxMs, Child->RttMaxUs,
            Child->RttLastUs, Child->Pongs);
    }

    for (i = 0; i < gOpt.Procs; i++)
    {
        if (gChildren[i].Process != NULL)
        {
            WaitForSingleObject(gChildren[i].Process, 5000);
            TerminateProcess(gChildren[i].Process, 0);
            CloseHandle(gChildren[i].Process);
        }
        if (gChildren[i].Pipe != NULL &&
            gChildren[i].Pipe != INVALID_HANDLE_VALUE)
        {
            CloseHandle(gChildren[i].Pipe);
        }
    }

    WaitForMultipleObjects(BeatCount, Beats, TRUE, 3000);
    WaitForSingleObject(Watchdog, 3000);

    Log("=== done ===\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Entry                                                               */
/* ------------------------------------------------------------------ */

static void Usage(void)
{
    printf(
    "stallmon - measure what stalls the desktop, not just reproduce it\n"
    "\n"
    "  --procs N      child processes (default 3)\n"
    "  --windows N    windows per child (default 3)\n"
    "  --render M     renderer 0..5, or -1 to vary per window (default -1)\n"
    "                 0 gdi  1 text  2 blit  3 stretch  4 fill  5 clip\n"
    "  --fps N        invalidations per second per window (default 30)\n"
    "  --storm        invalidate as fast as the loop turns, ignoring --fps\n"
    "  --nogui        no windows anywhere - CPU and IPC only\n"
    "  --spin PRI     also run a thread that never yields, at that priority\n"
    "                 (15 is where the anti-starvation boost gives up)\n"
    "  --ipc N        ms between cross-process pings (default 100)\n"
    "  --seconds N    how long to run (default 60)\n"
    "\n"
    "Chromium-shaped, from auditing base/ rather than guessing:\n"
    "  --hirestimer   ask for a 1ms system clock, as timeBeginPeriod(1) does.\n"
    "                 This changes the whole machine, not this process: the\n"
    "                 default here is 15ms, so it is ~15x the interrupt rate\n"
    "  --chromepump   drive the UI with Chromium's pump - a message-only\n"
    "                 window, kMsgHaveWork posts and\n"
    "                 MsgWaitForMultipleObjectsEx - instead of a plain loop\n"
    "  --pool N       N pool threads woken by events, each posting back to\n"
    "                 the UI thread the way a browser task does\n"
    "  --posthz N     cross-thread task posts per second (default 100)\n"
    "  --spinyield    contend a spin-then-yield lock, the shape a low level\n"
    "                 spinlock uses. A uniprocessor livelock suspect\n"
    "\n"
    "Everything is reported through the debug port, so read the results in\n"
    "the host's serial.log. A heartbeat that stops appearing is the finding.\n");
}

int main(int argc, char **argv)
{
    int i;

    /* Defaults */
    gOpt.Procs = 3;
    gOpt.Windows = 3;
    gOpt.Render = -1;
    gOpt.Fps = 30;
    gOpt.Seconds = 60;
    gOpt.IpcMs = 100;
    gOpt.PostHz = 100;

    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--child"))        gOpt.IsChild = TRUE;
        else if (!strcmp(argv[i], "--nogui"))   gOpt.NoGui = TRUE;
        else if (!strcmp(argv[i], "--storm"))   gOpt.Storm = TRUE;
        else if (!strcmp(argv[i], "--hirestimer")) gOpt.HiResTimer = TRUE;
        else if (!strcmp(argv[i], "--chromepump")) gOpt.ChromePump = TRUE;
        else if (!strcmp(argv[i], "--spinyield")) gOpt.SpinYield = TRUE;
        else if (!strcmp(argv[i], "--noyield")) gOpt.NoYield = TRUE;
        else if (!strcmp(argv[i], "--pool") && i + 1 < argc)
            gOpt.Pool = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--posthz") && i + 1 < argc)
            gOpt.PostHz = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--index") && i + 1 < argc)
            gOpt.Index = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--procs") && i + 1 < argc)
            gOpt.Procs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--windows") && i + 1 < argc)
            gOpt.Windows = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--render") && i + 1 < argc)
            gOpt.Render = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc)
            gOpt.Fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            gOpt.Seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--spin") && i + 1 < argc)
        {
            gOpt.SpinPriority = atoi(argv[++i]);
            gOpt.SpinEnabled = TRUE;
        }
        else if (!strcmp(argv[i], "--ipc") && i + 1 < argc)
            gOpt.IpcMs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pipe") && i + 1 < argc)
        {
            MultiByteToWideChar(CP_ACP, 0, argv[++i], -1,
                                gOpt.PipeName,
                                sizeof(gOpt.PipeName) / sizeof(WCHAR));
        }
        else
        {
            Usage();
            return 1;
        }
    }

    if (gOpt.Procs > MAX_PROCS)     gOpt.Procs = MAX_PROCS;
    if (gOpt.Windows > MAX_WINDOWS) gOpt.Windows = MAX_WINDOWS;

    LogInit();
    TimerResolutionInit();

    /* A child inherits the machine's resolution, so only the parent asks */
    return gOpt.IsChild ? ChildMain() : ParentMain();
}
