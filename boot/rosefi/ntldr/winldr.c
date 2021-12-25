/*
 * PROJECT:     ROSUEFI
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI NT loader core source file
 * COPYRIGHT:   Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#include "winldr.h"

EFI_STATUS
RefiLoadAndBootReactOSCommon(
    IN USHORT OperatingSystemVersion,
    IN PLOADER_PARAMETER_BLOCK LoaderBlock,
    IN PCSTR BootOptions,
    IN PCSTR BootPath)
{
    PLOADER_PARAMETER_BLOCK LoaderBlockVA;
    KERNEL_ENTRY_POINT KiSystemStartup;
    PLDR_DATA_TABLE_ENTRY KernelDTE;
    EFI_STATUS Success;
    PCSTR SystemRoot;

#ifdef _M_IX86
    /* Setup redirection support */
    //WinLdrSetupEms(BootOptions);
#endif

    /* Convert BootPath to SystemRoot */
    SystemRoot = strstr(BootPath, "\\");

    // - Detect Hardware (X86, ARM, ARM64, AMD64)

    Success = LoadWindowsCore(OperatingSystemVersion,
                              LoaderBlock,
                              BootOptions,
                              BootPath,
                              &KernelDTE);
    if(Success != EFI_SUCCESS)
    {
        /* Failure Logic */
    }
    
    // - Loader boot drivers with ldr
    // - Init loading phase 1

    /* Save entry-point pointer and Loader block VAs */
    KiSystemStartup = (KERNEL_ENTRY_POINT)KernelDTE->EntryPoint;
    // LoaderBlockVA = PaToVa(LoaderBlock); //TODO: Write Physical Address to Virtual Address translation
    // - Setup machine specific init
    // - Create memory descriptors
    // - Setup processor context.
    // - Save final value of LoaderPagesSpanned
    // - Zero out KI_USER_SHARED_DATA page

    // - Dump boot drivers and memory descriptors

    /* Start NTOSKRNL */
    (*KiSystemStartup)(LoaderBlockVA);
    /* UNIMPLEMENTED 12/24/2021 */
    UNREACHABLE;
    return Success;
}

static
EFI_STATUS
RefiLoadReactOSCore(IN USHORT OperatingSystemVersion,
                    IN OUT PLOADER_PARAMETER_BLOCK LoaderBlock,
                    IN PCSTR BootOptions,
                    IN PCSTR BootPath,
                    IN OUT PLDR_DATA_TABLE_ENTRY* KernelDTE)
{
    return EFI_SUCCESS;
}
