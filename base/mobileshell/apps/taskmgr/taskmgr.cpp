/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     Task Manager: processes with CPU and memory, system performance, end task
 */

#include <appclient.h>
#include <shellapi.h>
#include <strsafe.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <algorithm>
#include <map>

/* Per processor times from NtQuerySystemInformation, laid out as winternl.h documents them */
#define TASK_PROCESSOR_TIMES_CLASS 8

typedef struct _TASK_PROCESSOR_TIMES
{
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER Reserved1[2];
    ULONG Reserved2;
} TASK_PROCESSOR_TIMES;

extern "C"
LONG
NTAPI
NtQuerySystemInformation(
    _In_ ULONG InformationClass,
    _Out_writes_bytes_(Length) PVOID Information,
    _In_ ULONG Length,
    _Out_opt_ PULONG ReturnLength);

/*
 * Markup contract (taskmgr.uix):
 *   Elements: PageMain, PageProcess, TabProcessList, TabPerformance, ListScroll; repeaters ProcessList
 *             (fields Id, Name, Detail, Cpu, Glyph) and CoreGraphs (fields Label, Values)
 *   Bindings: TabProcesses, TabPerformance, ProcessSummary, CpuText, CpuHistory, CpuDetail,
 *             MemText, MemHistory, MemDetail, PerfProcesses, PerfThreads, PerfHandles,
 *             PerfCommit, PerfUptime, ProcName, ProcGlyph, ProcGone, ProcCpu, ProcMemory, ProcId,
 *             ProcCounts, ProcPriority, ProcParent, ProcPath
 *   Commands: tab:processes, tab:performance, open:<Pid>, hold:<Pid>
 */

#define TASKMGR_INTERVAL_MS 1000
#define TASKMGR_HISTORY 60

/* Keeps the newest TASKMGR_HISTORY samples */
static
VOID
PushSample(
    _Inout_ std::vector<FLOAT> &History,
    _In_ FLOAT Sample)
{
    History.push_back(Sample);
    if (History.size() > TASKMGR_HISTORY)
        History.erase(History.begin());
}

/* A history as the Graph element's Values list */
static
std::wstring
JoinSamples(
    _In_ const std::vector<FLOAT> &History)
{
    std::wstring Text;
    WCHAR Number[16];

    for (FLOAT Sample : History)
    {
        StringCchPrintfW(Number, ARRAYSIZE(Number), Text.empty() ? L"%.1f" : L",%.1f", Sample);
        Text += Number;
    }
    return Text;
}

typedef struct _PROCESS
{
    DWORD Pid = 0;
    DWORD Parent = 0;
    DWORD Session = 0;
    std::wstring Name;
    std::wstring Path;
    ULONG Threads = 0;
    ULONG Handles = 0;
    SIZE_T WorkingSet = 0;
    SIZE_T Private = 0;
    ULONGLONG CpuTime = 0;
    ULONGLONG Created = 0;
    DWORD Priority = 0;
    FLOAT Cpu = 0.0f;
} PROCESS;

typedef enum _TASK_PAGE
{
    PageMain,
    PageProcess
} TASK_PAGE;

typedef enum _TASK_SORT
{
    SortCpu,
    SortMemory,
    SortName
} TASK_SORT;

typedef enum _TASK_MENU
{
    MenuNone,
    MenuList,
    MenuProcess
} TASK_MENU;

static
ULONGLONG
FileTimeValue(
    _In_ const FILETIME &Time)
{
    return (static_cast<ULONGLONG>(Time.dwHighDateTime) << 32) | Time.dwLowDateTime;
}

static
std::wstring
FormatBytes(
    _In_ ULONGLONG Bytes)
{
    WCHAR Text[32];

    if (Bytes >= 1024ULL * 1024 * 1024)
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"%.1f GB", Bytes / (1024.0 * 1024.0 * 1024.0));
    else
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"%.1f MB", Bytes / (1024.0 * 1024.0));
    return Text;
}

static
PCWSTR
PriorityName(
    _In_ DWORD Priority)
{
    switch (Priority)
    {
        case IDLE_PRIORITY_CLASS: return L"Low";
        case BELOW_NORMAL_PRIORITY_CLASS: return L"Below normal";
        case NORMAL_PRIORITY_CLASS: return L"Normal";
        case ABOVE_NORMAL_PRIORITY_CLASS: return L"Above normal";
        case HIGH_PRIORITY_CLASS: return L"High";
        case REALTIME_PRIORITY_CLASS: return L"Realtime";
        default: return L"Unknown";
    }
}

/* Session 0 holds the services and the system, everything else is the user's */
static
PCWSTR
ProcessGlyph(
    _In_ const PROCESS &Process)
{
    return (Process.Session == 0) ? L"\xE770" : L"\xE7C4";
}

class CTaskManagerApp : public CAppClient
{
public:
    ~CTaskManagerApp()
    {
        if (m_Timer)
        {
            SetThreadpoolTimer(m_Timer, NULL, 0, 0);
            WaitForThreadpoolTimerCallbacks(m_Timer, TRUE);
            CloseThreadpoolTimer(m_Timer);
        }
    }

protected:
    VOID OnActivate(VOID) override;

    VOID OnWake(VOID) override;

    VOID OnCommand(
        _In_z_ PCWSTR Command) override;

    VOID OnHold(
        _In_z_ PCWSTR Command,
        _In_ FLOAT X,
        _In_ FLOAT Y) override;

    VOID OnSoftKey(
        _In_ SHELL_SOFTKEY Key) override;

    VOID OnCloseButton(VOID) override;

    VOID OnMenuPick(
        _In_ LONG Index) override;

private:
    static
    VOID
    CALLBACK
    TimerCallback(
        _Inout_ PTP_CALLBACK_INSTANCE Instance,
        _Inout_opt_ PVOID Context,
        _Inout_ PTP_TIMER Timer);

    VOID
    Sample(VOID);

    VOID
    SampleCores(VOID);

    VOID
    ShowPage(
        _In_ TASK_PAGE Page);

    VOID
    ShowTab(
        _In_ BOOL Performance);

    VOID
    RefreshList(VOID);

    VOID
    RefreshPerformance(VOID);

    VOID
    RefreshProcess(VOID);

    VOID
    EndTask(
        _In_ DWORD Pid);

    const PROCESS *
    FindProcess(
        _In_ DWORD Pid) const;

    std::vector<PROCESS> m_Processes;
    std::map<DWORD, std::pair<ULONGLONG, ULONGLONG>> m_LastCpu;
    std::vector<std::pair<ULONGLONG, ULONGLONG>> m_CoreLast;
    std::vector<std::vector<FLOAT>> m_CoreHistory;
    std::vector<FLOAT> m_CpuHistory;
    std::vector<FLOAT> m_MemHistory;
    PTP_TIMER m_Timer = NULL;
    ULONGLONG m_LastIdle = 0;
    ULONGLONG m_LastBusy = 0;
    ULONGLONG m_LastTotal = 0;
    FLOAT m_SystemCpu = 0.0f;
    MEMORYSTATUSEX m_Memory = {};
    PERFORMANCE_INFORMATION m_Performance = {};
    TASK_PAGE m_Page = PageMain;
    TASK_SORT m_Sort = SortCpu;
    TASK_MENU m_Menu = MenuNone;
    BOOL m_PerformanceTab = FALSE;
    DWORD m_Current = 0;
    DWORD m_HoldTarget = 0;
    FLOAT m_SeenPointerX = -1.0f;
    FLOAT m_SeenPointerY = -1.0f;
};

VOID
CALLBACK
CTaskManagerApp::TimerCallback(
    _Inout_ PTP_CALLBACK_INSTANCE Instance,
    _Inout_opt_ PVOID Context,
    _Inout_ PTP_TIMER Timer)
{
    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(Timer);

    static_cast<CTaskManagerApp *>(Context)->Wake();
}

VOID
CTaskManagerApp::OnActivate(VOID)
{
    LARGE_INTEGER Due;
    FILETIME DueTime;

    /* The first sample only sets the baselines, CPU shares need a second one */
    Sample();
    ShowPage(PageMain);

    if (!m_Timer)
    {
        m_Timer = CreateThreadpoolTimer(TimerCallback, this, NULL);
        if (m_Timer)
        {
            Due.QuadPart = -(LONGLONG)TASKMGR_INTERVAL_MS * 10000;
            DueTime.dwLowDateTime = Due.LowPart;
            DueTime.dwHighDateTime = (DWORD)Due.HighPart;
            SetThreadpoolTimer(m_Timer, &DueTime, TASKMGR_INTERVAL_MS, TASKMGR_INTERVAL_MS / 4);
        }
    }
}

VOID
CTaskManagerApp::OnWake(VOID)
{
    BOOL Touched;

    Sample();

    /* A list rebuilt under the finger loses its press, so a touch since the last tick waits one */
    Touched = (m_PointerX != m_SeenPointerX || m_PointerY != m_SeenPointerY);
    m_SeenPointerX = m_PointerX;
    m_SeenPointerY = m_PointerY;

    if (m_Page == PageProcess)
        RefreshProcess();
    else if (m_PerformanceTab)
        RefreshPerformance();
    else if (!Touched && m_Menu == MenuNone)
        RefreshList();
}

/**
 * @brief Takes one sample of every process and of the whole system.
 *
 * A process's CPU share is its CPU time since the last sample over the time all processors
 * spent since then, the same share Task Manager shows.
 */
VOID
CTaskManagerApp::Sample(VOID)
{
    std::map<DWORD, std::pair<ULONGLONG, ULONGLONG>> Cpu;
    PROCESS_MEMORY_COUNTERS_EX Counters;
    PROCESSENTRY32W Entry;
    FILETIME Idle, Kernel, User, Created, Exited, ProcessKernel, ProcessUser;
    ULONGLONG Total = 0;
    ULONGLONG Elapsed = 0;
    HANDLE Snapshot;
    HANDLE Process;
    WCHAR Path[MAX_PATH];
    DWORD Length;

    if (GetSystemTimes(&Idle, &Kernel, &User))
    {
        ULONGLONG IdleNow = FileTimeValue(Idle);
        ULONGLONG TotalNow = FileTimeValue(Kernel) + FileTimeValue(User);

        /* Kernel time includes the idle time */
        if (m_LastTotal && TotalNow > m_LastTotal)
        {
            Elapsed = TotalNow - m_LastTotal;
            m_SystemCpu = 100.0f * (Elapsed - min(Elapsed, IdleNow - m_LastIdle)) / Elapsed;
        }
        m_LastIdle = IdleNow;
        m_LastTotal = TotalNow;
        Total = Elapsed;
    }

    m_Memory.dwLength = sizeof(m_Memory);
    GlobalMemoryStatusEx(&m_Memory);

    /* The first sample is only a baseline for the CPU shares */
    if (Total)
    {
        PushSample(m_CpuHistory, m_SystemCpu);
        PushSample(m_MemHistory, static_cast<FLOAT>(m_Memory.dwMemoryLoad));
    }
    SampleCores();
    m_Performance.cb = sizeof(m_Performance);
    GetPerformanceInfo(&m_Performance, sizeof(m_Performance));

    Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (Snapshot == INVALID_HANDLE_VALUE)
        return;

    m_Processes.clear();
    Entry.dwSize = sizeof(Entry);
    for (BOOL More = Process32FirstW(Snapshot, &Entry); More; More = Process32NextW(Snapshot, &Entry))
    {
        PROCESS Item;

        if (Entry.th32ProcessID == 0)
            continue;

        Item.Pid = Entry.th32ProcessID;
        Item.Parent = Entry.th32ParentProcessID;
        Item.Name = Entry.szExeFile;
        Item.Threads = Entry.cntThreads;
        ProcessIdToSessionId(Item.Pid, &Item.Session);

        Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, Item.Pid);
        if (!Process)
            Process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, Item.Pid);
        if (!Process)
            Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Item.Pid);
        if (Process)
        {
            if (GetProcessTimes(Process, &Created, &Exited, &ProcessKernel, &ProcessUser))
            {
                Item.CpuTime = FileTimeValue(ProcessKernel) + FileTimeValue(ProcessUser);
                Item.Created = FileTimeValue(Created);
            }

            ZeroMemory(&Counters, sizeof(Counters));
            if (GetProcessMemoryInfo(Process, reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&Counters), sizeof(Counters)))
            {
                Item.WorkingSet = Counters.WorkingSetSize;
                Item.Private = Counters.PrivateUsage;
            }

            GetProcessHandleCount(Process, &Item.Handles);
            Item.Priority = GetPriorityClass(Process);

            Length = ARRAYSIZE(Path);
            if (QueryFullProcessImageNameW(Process, 0, Path, &Length))
                Item.Path = Path;
            CloseHandle(Process);
        }

        /* A reused process ID starts over, matched by its creation time */
        auto Last = m_LastCpu.find(Item.Pid);
        if (Total && Last != m_LastCpu.end() && Last->second.second == Item.Created && Item.CpuTime >= Last->second.first)
            Item.Cpu = min(100.0f, 100.0f * (Item.CpuTime - Last->second.first) / Total);
        Cpu[Item.Pid] = { Item.CpuTime, Item.Created };

        m_Processes.push_back(Item);
    }
    CloseHandle(Snapshot);
    m_LastCpu.swap(Cpu);
}

/**
 * @brief Samples every logical processor's busy share since the last sample.
 *
 * Kernel time includes idle time, so busy is kernel plus user minus idle.
 */
VOID
CTaskManagerApp::SampleCores(VOID)
{
    std::vector<TASK_PROCESSOR_TIMES> Cores;
    SYSTEM_INFO System;
    ULONG Length = 0;
    SIZE_T Index;

    GetSystemInfo(&System);
    Cores.resize(System.dwNumberOfProcessors);
    if (Cores.empty() ||
        NtQuerySystemInformation(TASK_PROCESSOR_TIMES_CLASS, Cores.data(),
                                 static_cast<ULONG>(Cores.size() * sizeof(Cores[0])), &Length) < 0)
    {
        return;
    }
    Cores.resize(Length / sizeof(Cores[0]));

    if (m_CoreLast.size() != Cores.size())
    {
        m_CoreLast.assign(Cores.size(), { 0, 0 });
        m_CoreHistory.assign(Cores.size(), std::vector<FLOAT>());
    }

    for (Index = 0; Index < Cores.size(); Index++)
    {
        ULONGLONG Idle = Cores[Index].IdleTime.QuadPart;
        ULONGLONG Total = Cores[Index].KernelTime.QuadPart + Cores[Index].UserTime.QuadPart;
        std::pair<ULONGLONG, ULONGLONG> &Last = m_CoreLast[Index];

        if (Last.second && Total > Last.second)
        {
            ULONGLONG Elapsed = Total - Last.second;
            ULONGLONG Rest = min(Elapsed, Idle - Last.first);

            PushSample(m_CoreHistory[Index], 100.0f * (Elapsed - Rest) / Elapsed);
        }
        else if (Last.second)
        {
            /* No time passed on this processor, it sat idle */
            PushSample(m_CoreHistory[Index], 0.0f);
        }
        Last = { Idle, Total };
    }
}

const PROCESS *
CTaskManagerApp::FindProcess(
    _In_ DWORD Pid) const
{
    for (const PROCESS &Process : m_Processes)
    {
        if (Process.Pid == Pid)
            return &Process;
    }
    return NULL;
}

VOID
CTaskManagerApp::ShowPage(
    _In_ TASK_PAGE Page)
{
    m_Page = Page;
    Scene()->SetVisible(L"PageMain", Page == PageMain);
    Scene()->SetVisible(L"PageProcess", Page == PageProcess);
    Scene()->SetSelected(NULL);

    if (Page == PageMain)
    {
        ShowTab(m_PerformanceTab);
        SetChrome(L"Task Manager", m_PerformanceTab ? L"" : L"Sort", L"Menu", ShellCloseOk);
    }
    else
    {
        RefreshProcess();
        SetChrome(L"Task Manager", L"End task", L"Menu", ShellCloseOk);
    }
}

VOID
CTaskManagerApp::ShowTab(
    _In_ BOOL Performance)
{
    m_PerformanceTab = Performance;
    Scene()->SetVisible(L"TabProcessList", !Performance);
    Scene()->SetVisible(L"TabPerformance", Performance);
    Scene()->SetBinding(L"TabProcesses", Performance ? L"@PivotOff" : L"@AppText");
    Scene()->SetBinding(L"TabPerformance", Performance ? L"@AppText" : L"@PivotOff");

    if (Performance)
        RefreshPerformance();
    else
        RefreshList();
}

VOID
CTaskManagerApp::RefreshList(VOID)
{
    static PCWSTR const Fields[] = { L"Id", L"Name", L"Detail", L"Cpu", L"Glyph" };
    static PCWSTR const SortNames[] = { L"CPU", L"memory", L"name" };
    std::vector<const PROCESS *> Order;
    std::vector<std::wstring> Storage;
    std::vector<PCWSTR> Values;
    WCHAR Text[128];

    for (const PROCESS &Process : m_Processes)
        Order.push_back(&Process);

    std::sort(Order.begin(), Order.end(), [this](const PROCESS *A, const PROCESS *B)
    {
        if (m_Sort == SortCpu && A->Cpu != B->Cpu)
            return A->Cpu > B->Cpu;
        if (m_Sort != SortName && A->WorkingSet != B->WorkingSet)
            return A->WorkingSet > B->WorkingSet;
        return _wcsicmp(A->Name.c_str(), B->Name.c_str()) < 0;
    });

    for (const PROCESS *Process : Order)
    {
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"%lu", Process->Pid);
        Storage.push_back(Text);
        Storage.push_back(Process->Name);
        Storage.push_back(L"Memory " + FormatBytes(Process->WorkingSet));
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"%.0f%%", Process->Cpu);
        Storage.push_back(Text);
        Storage.push_back(ProcessGlyph(*Process));
    }
    for (const std::wstring &Value : Storage)
        Values.push_back(Value.c_str());

    Scene()->SetItems(L"ProcessList", static_cast<ULONG>(Order.size()), ARRAYSIZE(Fields), Fields, Values.data());

    StringCchPrintfW(Text, ARRAYSIZE(Text), L"%lu processes, CPU %.0f%%, by %s",
                     static_cast<ULONG>(m_Processes.size()), m_SystemCpu, SortNames[m_Sort]);
    Scene()->SetBinding(L"ProcessSummary", Text);
}

VOID
CTaskManagerApp::RefreshPerformance(VOID)
{
    ULONGLONG Used = m_Memory.ullTotalPhys - m_Memory.ullAvailPhys;
    ULONGLONG Seconds = GetTickCount64() / 1000;
    SYSTEM_INFO System;
    WCHAR Text[96];

    static PCWSTR const CoreFields[] = { L"Label", L"Values" };
    std::vector<std::wstring> Storage;
    std::vector<PCWSTR> Values;

    StringCchPrintfW(Text, ARRAYSIZE(Text), L"%.0f%%", m_SystemCpu);
    Scene()->SetBinding(L"CpuText", Text);
    Scene()->SetBinding(L"CpuHistory", JoinSamples(m_CpuHistory).c_str());

    for (SIZE_T Index = 0; Index < m_CoreHistory.size(); Index++)
    {
        const std::vector<FLOAT> &History = m_CoreHistory[Index];

        StringCchPrintfW(Text, ARRAYSIZE(Text), L"CPU %lu, %.0f%%", static_cast<ULONG>(Index),
                         History.empty() ? 0.0f : History.back());
        Storage.push_back(Text);
        Storage.push_back(JoinSamples(History));
    }
    for (const std::wstring &Value : Storage)
        Values.push_back(Value.c_str());
    Scene()->SetItems(L"CoreGraphs", static_cast<ULONG>(m_CoreHistory.size()), ARRAYSIZE(CoreFields), CoreFields, Values.data());
    GetSystemInfo(&System);
    StringCchPrintfW(Text, ARRAYSIZE(Text), L"%lu logical processors", System.dwNumberOfProcessors);
    Scene()->SetBinding(L"CpuDetail", Text);

    StringCchPrintfW(Text, ARRAYSIZE(Text), L"%lu%%", m_Memory.dwMemoryLoad);
    Scene()->SetBinding(L"MemText", Text);
    Scene()->SetBinding(L"MemHistory", JoinSamples(m_MemHistory).c_str());
    Scene()->SetBinding(L"MemDetail", (FormatBytes(Used) + L" in use of " + FormatBytes(m_Memory.ullTotalPhys) +
                                       L", " + FormatBytes(m_Memory.ullAvailPhys) + L" available").c_str());

    StringCchPrintfW(Text, ARRAYSIZE(Text), L"%lu", m_Performance.ProcessCount);
    Scene()->SetBinding(L"PerfProcesses", Text);
    StringCchPrintfW(Text, ARRAYSIZE(Text), L"%lu", m_Performance.ThreadCount);
    Scene()->SetBinding(L"PerfThreads", Text);
    StringCchPrintfW(Text, ARRAYSIZE(Text), L"%lu", m_Performance.HandleCount);
    Scene()->SetBinding(L"PerfHandles", Text);
    Scene()->SetBinding(L"PerfCommit", (FormatBytes(static_cast<ULONGLONG>(m_Performance.CommitTotal) * m_Performance.PageSize) +
                                        L" of " + FormatBytes(static_cast<ULONGLONG>(m_Performance.CommitLimit) * m_Performance.PageSize)).c_str());
    StringCchPrintfW(Text, ARRAYSIZE(Text), L"%llu:%02llu:%02llu:%02llu",
                     Seconds / 86400, (Seconds / 3600) % 24, (Seconds / 60) % 60, Seconds % 60);
    Scene()->SetBinding(L"PerfUptime", Text);
}

VOID
CTaskManagerApp::RefreshProcess(VOID)
{
    const PROCESS *Process = FindProcess(m_Current);
    const PROCESS *Parent;
    WCHAR Text[160];

    if (!Process)
    {
        Scene()->SetBinding(L"ProcGone", L"This process has ended.");
        return;
    }

    Parent = FindProcess(Process->Parent);
    Scene()->SetBinding(L"ProcGone", L"");
    Scene()->SetBinding(L"ProcName", Process->Name.c_str());
    Scene()->SetBinding(L"ProcGlyph", ProcessGlyph(*Process));
    StringCchPrintfW(Text, ARRAYSIZE(Text), L"%.1f%%", Process->Cpu);
    Scene()->SetBinding(L"ProcCpu", Text);
    Scene()->SetBinding(L"ProcMemory", (FormatBytes(Process->WorkingSet) + L", " + FormatBytes(Process->Private)).c_str());
    StringCchPrintfW(Text, ARRAYSIZE(Text), L"%lu, session %lu", Process->Pid, Process->Session);
    Scene()->SetBinding(L"ProcId", Text);
    StringCchPrintfW(Text, ARRAYSIZE(Text), L"%lu threads, %lu handles", Process->Threads, Process->Handles);
    Scene()->SetBinding(L"ProcCounts", Text);
    Scene()->SetBinding(L"ProcPriority", PriorityName(Process->Priority));
    if (Parent)
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"%s (%lu)", Parent->Name.c_str(), Parent->Pid);
    else
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"Process %lu, no longer running", Process->Parent);
    Scene()->SetBinding(L"ProcParent", Text);
    Scene()->SetBinding(L"ProcPath", Process->Path.empty() ? L"Not available" : Process->Path.c_str());
}

/**
 * @brief Ends a process the way Task Manager's End task does, without asking it first.
 */
VOID
CTaskManagerApp::EndTask(
    _In_ DWORD Pid)
{
    HANDLE Process;
    WCHAR Text[160];
    DWORD Error = ERROR_SUCCESS;

    if (Pid == GetCurrentProcessId())
    {
        RequestClose();
        return;
    }

    Process = OpenProcess(PROCESS_TERMINATE, FALSE, Pid);
    if (!Process || !TerminateProcess(Process, 1))
        Error = GetLastError();
    if (Process)
        CloseHandle(Process);

    if (Error != ERROR_SUCCESS)
    {
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"Could not end process %lu, error %lu.%s", Pid, Error,
                         (Error == ERROR_ACCESS_DENIED) ? L" It needs administrator rights or is protected." : L"");
        Notify(L"Task Manager", Text);
    }

    Sample();
    if (m_Page == PageProcess && Pid == m_Current)
        ShowPage(PageMain);
    else if (m_Page == PageMain && !m_PerformanceTab)
        RefreshList();
}

VOID
CTaskManagerApp::OnCommand(
    _In_z_ PCWSTR Command)
{
    if (_wcsicmp(Command, L"tab:processes") == 0 || _wcsicmp(Command, L"tab:performance") == 0)
    {
        ShowTab(_wcsicmp(Command + 4, L"performance") == 0);
        SetChrome(L"Task Manager", m_PerformanceTab ? L"" : L"Sort", L"Menu", ShellCloseOk);
    }
    else if (_wcsnicmp(Command, L"open:", 5) == 0)
    {
        m_Current = wcstoul(Command + 5, NULL, 10);
        ShowPage(PageProcess);
    }
    else
    {
        CAppClient::OnCommand(Command);
    }
}

VOID
CTaskManagerApp::OnHold(
    _In_z_ PCWSTR Command,
    _In_ FLOAT X,
    _In_ FLOAT Y)
{
    if (_wcsnicmp(Command, L"hold:", 5) != 0)
        return;

    m_HoldTarget = wcstoul(Command + 5, NULL, 10);
    m_Menu = MenuProcess;
    ShowMenu(AppMenuPoint, X, Y, L"End task\n-\nDetails");
}

VOID
CTaskManagerApp::OnSoftKey(
    _In_ SHELL_SOFTKEY Key)
{
    if (m_Page == PageProcess)
    {
        if (Key == ShellSoftKeyLeft)
        {
            EndTask(m_Current);
        }
        else if (Key == ShellSoftKeyRight)
        {
            m_HoldTarget = m_Current;
            m_Menu = MenuProcess;
            ShowMenu(AppMenuRightSoftKey, 0.0f, 0.0f, L"End task");
        }
        return;
    }

    if (Key == ShellSoftKeyLeft && !m_PerformanceTab)
    {
        m_Sort = static_cast<TASK_SORT>((m_Sort + 1) % 3);
        Scene()->ResetScroll(L"ListScroll");
        RefreshList();
    }
    else if (Key == ShellSoftKeyRight)
    {
        m_Menu = MenuList;
        ShowMenu(AppMenuRightSoftKey, 0.0f, 0.0f, L"Sort by CPU\nSort by memory\nSort by name\n-\nPerformance\nProcesses");
    }
}

VOID
CTaskManagerApp::OnCloseButton(VOID)
{
    if (m_Page == PageProcess)
        ShowPage(PageMain);
    else
        RequestClose();
}

VOID
CTaskManagerApp::OnMenuPick(
    _In_ LONG Index)
{
    TASK_MENU Menu = m_Menu;

    m_Menu = MenuNone;
    if (Index < 0)
        return;

    if (Menu == MenuList)
    {
        if (Index <= 2)
        {
            m_Sort = static_cast<TASK_SORT>(Index);
            if (m_PerformanceTab)
                ShowTab(FALSE);
            Scene()->ResetScroll(L"ListScroll");
            RefreshList();
        }
        else
        {
            ShowTab(Index == 3);
        }
        SetChrome(L"Task Manager", m_PerformanceTab ? L"" : L"Sort", L"Menu", ShellCloseOk);
    }
    else if (Menu == MenuProcess)
    {
        if (Index == 0)
        {
            EndTask(m_HoldTarget);
        }
        else if (Index == 1)
        {
            m_Current = m_HoldTarget;
            ShowPage(PageProcess);
        }
    }
}

INT
WINAPI
wWinMain(
    _In_ HINSTANCE Instance,
    _In_opt_ HINSTANCE Previous,
    _In_ PWSTR CommandLine,
    _In_ INT ShowCommand)
{
    CTaskManagerApp App;
    PWSTR *Arguments;
    INT Count = 0;
    INT Result = 1;

    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(Previous);
    UNREFERENCED_PARAMETER(CommandLine);
    UNREFERENCED_PARAMETER(ShowCommand);

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    Arguments = CommandLineToArgvW(GetCommandLineW(), &Count);
    if (Arguments && SUCCEEDED(App.Initialize(Count, Arguments)))
        Result = App.Run();
    LocalFree(Arguments);
    CoUninitialize();
    return Result;
}
