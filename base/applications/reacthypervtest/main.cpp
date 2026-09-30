/*
 * PROJECT:     ReactHypervTest
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Driving a machine from a terminal, and saying what it did
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * The machine itself is next door. This is the front end that runs one to the
 * end without being watched, which is what is wanted when the question is
 * whether a thing works at all rather than what it looks like doing it.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "vm.h"

static void Usage()
{
    printf(
        "ReactHypervTest, which drives real hardware through our contract\n"
        "\n"
        "  reacthypervtest --library <file> --class <guid>\n"
        "  reacthypervtest --firmware <file> [--memory <bytes>]\n"
        "  reacthypervtest --bios <file> --device <file>,<guid>[,<settings>]\n"
        "\n"
        "  --library <file>   The class server to load, which may be one of\n"
        "                     the real ones\n"
        "  --class <guid>     Which kind of device to ask it for\n"
        "  --firmware <file>  A firmware to run on a machine with nothing in it\n"
        "  --bios <file>      A firmware to run against the devices below\n"
        "  --device <f>,<g>   A kind of device to put in the machine, repeated.\n"
        "                     Anything past a second comma is handed to that\n"
        "                     kind for itself, which is how a disc is named\n"
        "  --memory <bytes>   How much memory the machine has\n"
        "  --steps <n>        How many times to stop before giving up\n"
        "  --stand-in         Hand a device a logged stand-in for any service\n"
        "                     this does not have, rather than refusing it\n"
        "  --dump <file>      Write the memory out when the run is over\n"
        "  --said <file>      Write everything said on the first serial port\n"
        "  --press <n>:<s>    Press scan code <s> once <n> stops have gone by,\n"
        "                     repeated. Prefix the code with e0 for the ones a\n"
        "                     keyboard sends two bytes for\n"
        "  --watch-faults     Stop the processor on the faults the guest takes\n"
        "\n"
        "What the device asks for on the way up is printed. That list is what\n"
        "has to exist before the real hardware will run against this.\n");
}

int main(int argc, char **argv)
{
    const char *Library = nullptr;
    const char *Class = nullptr;
    const char *Firmware = nullptr;
    const char *Bios = nullptr;
    const char *Media = nullptr;
    ULONG Steps = 20000;
    ULONG64 Ram = 0;

    Part Parts[MACHINE_PARTS] = {};
    ULONG Many = 0;

    /*
     * Said the moment it is said. What is being driven here is somebody else's
     * and may stop the whole process rather than return, and anything still
     * waiting to be written when that happens is the part worth reading.
     */
    setvbuf(stdout, nullptr, _IONBF, 0);

    for (int Index = 1; Index < argc; Index++)
    {
        if ((strcmp(argv[Index], "--library") == 0) && ((Index + 1) < argc))
            Library = argv[++Index];
        else if ((strcmp(argv[Index], "--class") == 0) && ((Index + 1) < argc))
            Class = argv[++Index];
        else if (strcmp(argv[Index], "--store-refuses") == 0)
            VmStoreAnswers(E_NOTIMPL);
        else if (strcmp(argv[Index], "--stand-in") == 0)
            VmAllowStandIns(true);
        else if ((strcmp(argv[Index], "--dump") == 0) && ((Index + 1) < argc))
            VmDump(argv[++Index]);
        else if ((strcmp(argv[Index], "--said") == 0) && ((Index + 1) < argc))
            VmSaidTo(argv[++Index]);
        else if ((strcmp(argv[Index], "--press") == 0) && ((Index + 1) < argc))
        {
            /* How far in, then which key, because the wait is the hard part */
            char *Text = argv[++Index];
            char *Colon = strchr(Text, ':');

            if (Colon == nullptr)
            {
                Usage();
                return 1;
            }

            *Colon = '\0';

            const ULONG64 After = strtoull(Text, nullptr, 0);
            const ULONG Code = (ULONG)strtoul(Colon + 1, nullptr, 16);

            VmPress(After, (USHORT)(Code & 0xFF), (Code & 0xE000) == 0xE000);
        }
        else if (strcmp(argv[Index], "--watch-faults") == 0)
            VmFaults(true);
        else if ((strcmp(argv[Index], "--settings") == 0) && ((Index + 1) < argc))
            Media = argv[++Index];
        else if ((strcmp(argv[Index], "--watch") == 0) && ((Index + 2) < argc))
        {
            const ULONG First = (ULONG)strtoul(argv[++Index], nullptr, 0);

            VmWatch(First, (ULONG)strtoul(argv[++Index], nullptr, 0));
        }
        else if ((strcmp(argv[Index], "--configuration") == 0) && ((Index + 1) < argc))
        {
            /* Read whole, because what it says is one document */
            FILE *File = fopen(argv[++Index], "rb");
            static char Xml[8192];

            if (File == nullptr)
            {
                printf("%s would not open\n", argv[Index]);
                return 1;
            }

            const size_t Read = fread(Xml, 1, sizeof(Xml) - 1, File);

            fclose(File);
            Xml[Read] = 0;
            VmConfiguration(Xml);
        }
        else if ((strcmp(argv[Index], "--firmware") == 0) && ((Index + 1) < argc))
            Firmware = argv[++Index];
        else if ((strcmp(argv[Index], "--steps") == 0) && ((Index + 1) < argc))
            Steps = (ULONG)strtoul(argv[++Index], nullptr, 0);
        else if ((strcmp(argv[Index], "--memory") == 0) && ((Index + 1) < argc))
            Ram = strtoull(argv[++Index], nullptr, 0);
        else if ((strcmp(argv[Index], "--bios") == 0) && ((Index + 1) < argc))
            Bios = argv[++Index];
        else if ((strcmp(argv[Index], "--device") == 0) && ((Index + 1) < argc))
        {
            /* Where the library ends and the kind begins, split in place */
            char *Text = argv[++Index];
            char *Comma = strchr(Text, ',');

            if ((Comma == nullptr) || (Many >= MACHINE_PARTS))
            {
                Usage();
                return 1;
            }

            *Comma = '\0';
            Parts[Many].Library = Text;
            Parts[Many].Class = Comma + 1;

            /* And whatever is past the kind, which is the kind's own business */
            char *Second = strchr(Comma + 1, ',');

            if (Second != nullptr)
            {
                *Second = '\0';
                Parts[Many].Settings = Second + 1;
            }

            Many++;
        }
        else
        {
            Usage();
            return 1;
        }
    }

    if (Bios != nullptr)
    {
        return Assemble(Bios, Parts, Many,
                        (Ram != 0) ? Ram : 0x08000000ull, Steps);
    }

    if (Firmware != nullptr)
        return hv::Run(Firmware, Steps, Ram);

    if ((Library == nullptr) || (Class == nullptr))
    {
        Usage();
        return 1;
    }

    return One(Library, Class, Media);
}
