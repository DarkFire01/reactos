/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     What the operator types, and where the log comes out
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "rtvmm.h"

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
        "  --quiet              Only say what went wrong\n"
        "  --verbose            Say everything\n"
        "\n"
        "Settings are separated by commas and mean whatever the kind decides.\n"
        "The serial port takes com1 or com2, port=<n>, line=<n>, and one of\n"
        "pipe=<name> or file=<path> for where its bytes go.\n"
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
                                (strcmp(Argument, "--seconds") == 0);

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

int main(int argc, char **argv)
{
    rtvm::Configuration Config;

    if (!rtvm::ParseCommandLine(argc, argv, Config))
        return 1;

    /*
     * On the heap rather than the stack: the port table alone is half a
     * megabyte, and the default stack is not a great deal more than that.
     */
    rtvm::Owned<rtvm::Machine> Machine(new rtvm::Machine());

    if (!Machine || !Machine->Build(Config))
        return 1;

    RunningMachine = Machine.Get();
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    const rtvm::StopReason Reason = Machine->Run();

    RunningMachine = nullptr;
    SetConsoleCtrlHandler(ConsoleHandler, FALSE);

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
