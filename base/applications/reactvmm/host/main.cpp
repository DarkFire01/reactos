/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     What the operator types, and where the log comes out
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "panel.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace rtvm
{

static RTVM_LOG_LEVEL LogLevel = RtvmLogInfo;

void SetLogLevel(RTVM_LOG_LEVEL Level)
{
    LogLevel = Level;
}

void Log(RTVM_LOG_LEVEL Level, const char *Format, ...)
{
    static const char *const Prefix[] = { "error", "warning", "", "trace" };
    va_list Arguments;

    if (Level > LogLevel)
        return;

    if (Prefix[Level][0] != '\0')
        printf("%s: ", Prefix[Level]);

    va_start(Arguments, Format);
    vprintf(Format, Arguments);
    va_end(Arguments);

    fflush(stdout);
}

void PrintUsage()
{
    printf(
        "ReacTVmm, a machine built out of the hypervisor platform\n"
        "\n"
        "  reactvmm --firmware <file> [options]\n"
        "\n"
        "  --firmware <file>    The image to come out of reset into\n"
        "  --memory <mb>        How much memory the machine has, 128 by default\n"
        "  --processors <n>     How many processors it has, 1 by default\n"
        "  --device <kind>[:<settings>]\n"
        "                       Add a piece of hardware, more than once if wanted\n"
        "  --seconds <n>        Stop of its own accord after this long\n"
        "  --window             Open a window and watch the machine in it\n"
        "  --capture <file>     Leave a picture of that window behind when it goes\n"
        "  --dump <base>:<len>,<file>\n"
        "                       Write out that much of the machine's memory at the\n"
        "                       end, both numbers in hex\n"
        "  --keys <wait>:<code>[,...]\n"
        "                       Press keys at it once it is running, waiting that\n"
        "                       many milliseconds before each. A code is what the\n"
        "                       wire carries, in hex, with e0 in front where the\n"
        "                       wire puts it: e050 is the key that moves down.\n"
        "  --quiet              Only say what went wrong\n"
        "  --verbose            Say everything\n"
        "\n"
        "Settings are separated by commas and mean whatever the kind decides.\n"
        "The serial port takes com1 or com2, port=<n>, line=<n>, and one of\n"
        "pipe=<name> or file=<path> for where its bytes go. A pipe also takes\n"
        "wait, which holds the machine until something plugs into the other end\n"
        "so that nothing it says while coming up is said to nobody.\n"
        "\n"
        "  reactvmm --firmware rtvmbios.bin --device serial:com1,pipe=rtvm-com1\n");
}

bool ParseCommandLine(int argc, char **argv, Configuration &Config)
{
    for (int Index = 1; Index < argc; Index++)
    {
        const char *Argument = argv[Index];
        const bool NeedsValue = (strcmp(Argument, "--firmware") == 0) ||
                                (strcmp(Argument, "--memory") == 0) ||
                                (strcmp(Argument, "--processors") == 0) ||
                                (strcmp(Argument, "--device") == 0) ||
                                (strcmp(Argument, "--seconds") == 0) ||
                                (strcmp(Argument, "--capture") == 0) ||
                                (strcmp(Argument, "--dump") == 0) ||
                                (strcmp(Argument, "--keys") == 0) ||
                                (strcmp(Argument, "--watch") == 0) ||
                                (strcmp(Argument, "--see") == 0);

        if (NeedsValue && (Index + 1 >= argc))
        {
            printf("%s wants a value after it\n", Argument);
            return false;
        }

        if (strcmp(Argument, "--firmware") == 0)
        {
            if (!Config.FirmwarePath.Set(argv[++Index]))
            {
                printf("that path is longer than this can hold\n");
                return false;
            }
        }
        else if (strcmp(Argument, "--memory") == 0)
        {
            const unsigned long Megabytes = strtoul(argv[++Index], nullptr, 0);

            if ((Megabytes < 2) || (Megabytes > 4096))
            {
                printf("a machine of %lu MB is not one this can build\n", Megabytes);
                return false;
            }

            Config.MemorySize = (ULONG64)Megabytes * 1024 * 1024;
        }
        else if (strcmp(Argument, "--processors") == 0)
        {
            Config.ProcessorCount = strtoul(argv[++Index], nullptr, 0);

            if ((Config.ProcessorCount < 1) || (Config.ProcessorCount > 64))
            {
                printf("%lu processors is not a number this can build\n",
                       Config.ProcessorCount);
                return false;
            }
        }
        else if (strcmp(Argument, "--device") == 0)
        {
            DeviceRequest Request;

            if (!Request.Set(argv[++Index]))
            {
                printf("%s is longer than a device request may be\n", argv[Index]);
                return false;
            }

            if (!Config.Requests.Add(Request))
            {
                printf("that is more hardware than this can build\n");
                return false;
            }
        }
        else if (strcmp(Argument, "--seconds") == 0)
        {
            Config.RunSeconds = strtoul(argv[++Index], nullptr, 0);
        }
        else if (strcmp(Argument, "--window") == 0)
        {
            Config.Window = true;
        }
        else if (strcmp(Argument, "--capture") == 0)
        {
            if (!Config.CapturePath.Set(argv[++Index]))
            {
                printf("that path is longer than this can hold\n");
                return false;
            }

            Config.Window = true;
        }
        else if (strcmp(Argument, "--dump") == 0)
        {
            /* Where, how much, and what to call it, as base:length,path */
            const char *Text = argv[++Index];
            char *After = nullptr;

            Config.DumpBase = _strtoui64(Text, &After, 16);

            if ((After == nullptr) || (*After != ':'))
            {
                printf("a dump is written as base:length,path\n");
                return false;
            }

            Config.DumpLength = strtoul(After + 1, &After, 16);

            if ((After == nullptr) || (*After != ',') ||
                !Config.DumpPath.Set(After + 1))
            {
                printf("a dump is written as base:length,path\n");
                return false;
            }
        }
        else if (strcmp(Argument, "--keys") == 0)
        {
            if (!Config.Keys.Set(argv[++Index]))
            {
                printf("that many keys is more than this can hold\n");
                return false;
            }
        }
        else if (strcmp(Argument, "--watch") == 0)
        {
            /* A run of ports, written as first-last in hex */
            const char *Text = argv[++Index];
            char *After = nullptr;

            Config.WatchFirst = (USHORT)strtoul(Text, &After, 16);

            if ((After != nullptr) && (*After == '-'))
                Config.WatchLast = (USHORT)strtoul(After + 1, &After, 16);
            else
                Config.WatchLast = Config.WatchFirst;

            if (Config.WatchLast < Config.WatchFirst)
            {
                printf("a watch is written as first-last\n");
                return false;
            }
        }
        else if (strcmp(Argument, "--see") == 0)
        {
            /* A window, written as first-last in hex */
            const char *Text = argv[++Index];
            char *After = nullptr;

            Config.SeeFirst = _strtoui64(Text, &After, 16);

            if ((After != nullptr) && (*After == '-'))
                Config.SeeLast = _strtoui64(After + 1, &After, 16);
            else
                Config.SeeLast = Config.SeeFirst;

            if (Config.SeeLast < Config.SeeFirst)
            {
                printf("a window is written as first-last\n");
                return false;
            }
        }
        else if (strcmp(Argument, "--quiet") == 0)
        {
            Config.LogLevel = RtvmLogError;
        }
        else if (strcmp(Argument, "--verbose") == 0)
        {
            Config.LogLevel = RtvmLogTrace;
        }
        else if ((strcmp(Argument, "--help") == 0) || (strcmp(Argument, "-h") == 0))
        {
            PrintUsage();
            return false;
        }
        else
        {
            printf("%s is not something this understands\n", Argument);
            return false;
        }
    }

    if (Config.FirmwarePath.Empty())
    {
        PrintUsage();
        return false;
    }

    return true;
}

} /* namespace rtvm */

/* The machine the handler stops, which is why it is reachable from there */
static rtvm::Machine *RunningMachine = nullptr;

static BOOL WINAPI ConsoleHandler(DWORD Event)
{
    UNREFERENCED_PARAMETER(Event);

    if (RunningMachine != nullptr)
        RunningMachine->Stop();

    return TRUE;
}

/* What the thread that runs the machine needs while the window is drawing */
struct RunRequest
{
    rtvm::Machine *Machine;
    HWND Window;
    rtvm::StopReason Reason;
};

/* And what the one pressing keys at it needs */
struct TypeRequest
{
    rtvm::Machine *Machine;
    const char *Keys;
};

/**
 * @brief
 * Presses the keys the operator asked for, at the times they asked for.
 *
 * @remarks
 * Each is pressed and let go, because a guest that was only told a key went
 * down waits for it to come back up before it will take another.
 */
static DWORD WINAPI TypeThread(LPVOID Parameter)
{
    auto *Request = static_cast<TypeRequest *>(Parameter);
    const char *At = Request->Keys;

    while ((At != nullptr) && (*At != '\0'))
    {
        char *After = nullptr;
        const ULONG Wait = strtoul(At, &After, 10);

        if ((After == nullptr) || (*After != ':'))
            break;

        const ULONG Code = strtoul(After + 1, &After, 16);

        Sleep(Wait);

        /* The same code both ways, because what marks a release is not ours */
        Request->Machine->PostInput(RtvmInputKeyDown, Code);
        Sleep(30);
        Request->Machine->PostInput(RtvmInputKeyUp, Code);

        if ((After == nullptr) || (*After != ','))
            break;

        At = After + 1;
    }

    return 0;
}

/*
 * The machine runs here while the window pumps messages. The processor blocks
 * inside the platform library for as long as the guest keeps going, so the two
 * cannot share a thread, and the window is the one that has to stay answering.
 */
static DWORD WINAPI RunThread(LPVOID Parameter)
{
    auto *Request = static_cast<RunRequest *>(Parameter);

    Request->Reason = Request->Machine->Run();

    /*
     * The window stays up afterwards so the last screen can be read, unless it
     * has already gone, in which case there is nothing to tell.
     */
    if (IsWindow(Request->Window))
        PostMessageA(Request->Window, WM_APP, 0, 0);

    return 0;
}

int main(int argc, char **argv)
{
    rtvm::Configuration Config;

    if (!rtvm::ParseCommandLine(argc, argv, Config))
        return 1;

    /*
     * On the heap rather than the stack: the port table alone is half a
     * megabyte, and the default stack is not a great deal more than that.
     */
    /*
     * The window is declared before the machine and opened before it is built.
     * A device asks for whoever is looking while it comes up, and holds on to
     * it until it goes; a window that went first would be let go of after it
     * had already gone.
     */
    rtvm::Owned<rtvm::Panel> Front;

    rtvm::Owned<rtvm::Machine> Machine(new rtvm::Machine());

    if (!Machine)
        return 1;

    if (Config.Window)
    {
        Front.Reset(new rtvm::Panel());

        if (!Front || !Front->Open(*Machine, "ReacTVmm"))
            return 1;

        Machine->Watch(Front.Get());
        Front->CloseWhenStopped(Config.RunSeconds != 0);

        if (!Config.CapturePath.Empty())
            Front->CaptureTo(Config.CapturePath.Get());
    }

    if (!Machine->Build(Config))
        return 1;

    RunningMachine = Machine.Get();
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    TypeRequest Typing = { Machine.Get(), Config.Keys.Get() };
    HANDLE Typist = nullptr;

    if (!Config.Keys.Empty())
        Typist = CreateThread(nullptr, 0, TypeThread, &Typing, 0, nullptr);

    rtvm::StopReason Reason;

    if (Config.Window)
    {
        RunRequest Request = { Machine.Get(), Front->Window(),
                               rtvm::StopReason::Cancelled };
        HANDLE Thread = CreateThread(nullptr, 0, RunThread, &Request, 0, nullptr);

        if (Thread == nullptr)
        {
            rtvm::Log(RtvmLogError, "the machine could not be given a thread\n");
            return 1;
        }

        Front->Pump();

        /* Closing the window stopped it, so this is only waiting for it to go */
        Machine->Stop();
        WaitForSingleObject(Thread, INFINITE);
        CloseHandle(Thread);

        Reason = Request.Reason;
    }
    else
    {
        Reason = Machine->Run();
    }

    RunningMachine = nullptr;
    SetConsoleCtrlHandler(ConsoleHandler, FALSE);

    if (Typist != nullptr)
    {
        WaitForSingleObject(Typist, 5000);
        CloseHandle(Typist);
    }

    if (!Config.DumpPath.Empty())
    {
        Machine->DumpMemory(Config.DumpPath.Get(), Config.DumpBase,
                            Config.DumpLength);
    }

    switch (Reason)
    {
        case rtvm::StopReason::Halted:
        case rtvm::StopReason::Shutdown:
        case rtvm::StopReason::Cancelled:
            rtvm::Log(RtvmLogInfo, "stopped\n");
            return 0;

        case rtvm::StopReason::TripleFault:
            rtvm::Log(RtvmLogError, "the machine faulted its way to a stop\n");
            return 2;

        default:
            rtvm::Log(RtvmLogError, "the machine stopped early\n");
            return 3;
    }
}
