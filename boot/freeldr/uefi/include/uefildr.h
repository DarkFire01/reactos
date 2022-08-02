/*
 * PROJECT:     Freeldr UEFI Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Uefi freeldr core header
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/* INCLUDES ******************************************************************/
#include <freeldr.h>

/* UEFI Headers */
#include <efi/Uefi.h>
#include <efi/DevicePath.h>
#include <efi/LoadedImage.h>
#include <efi/GraphicsOutput.h>
#include <efi/UgaDraw.h>
#include <efi/BlockIo.h>
#include <efi/Acpi.h>
#include <efi/GlobalVariable.h>

#include "machuefi.h"

VOID
DebugInit(IN ULONG_PTR FrLdrSectionId);

VOID
UefiItoa(ULONG64, CHAR16* buffer, int basenumber);

UINT32 
UefiStrlen(PUCHAR str);

VOID
UefiConsSetCursor(UINT32 Col, UINT32 Row);

VOID
UefiIncrement();

/* kbd shit */

VOID
UefiInitializeInputSupport(_In_ EFI_HANDLE ImageHandle,
                                _In_ EFI_SYSTEM_TABLE *SystemTable);


VOID
UefiConsoleReset();

VOID
UefiWaitForAnyKey();

VOID
UefiPollAndDrawKeyboardInput();


VOID
UefiMemInit(_In_ EFI_HANDLE ImageHandle,
             _In_ EFI_SYSTEM_TABLE *SystemTable);


             ULONG
AddMemoryDescriptor(
    IN OUT PFREELDR_MEMORY_DESCRIPTOR List,
    IN ULONG MaxCount,
    IN PFN_NUMBER BasePage,
    IN PFN_NUMBER PageCount,
    IN TYPE_OF_MEMORY MemoryType);