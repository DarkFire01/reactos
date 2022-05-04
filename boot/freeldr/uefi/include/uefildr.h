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

VOID
DebugInit(IN ULONG_PTR FrLdrSectionId);

VOID
UefiInitConsole(_In_ EFI_SYSTEM_TABLE *SystemTable);

VOID
UefiConsPutChar(int Ch);

VOID
UefiInitalizeVideo(_In_ EFI_HANDLE ImageHandle,
                   _In_ EFI_SYSTEM_TABLE *SystemTable,
                   _In_ EFI_GRAPHICS_OUTPUT_PROTOCOL* gop);

VOID
RefiDrawUIBackground();

VOID
UefiVideoGetDisplaySize(PULONG Width, PULONG Height, PULONG Depth);

VIDEODISPLAYMODE
UefiVideoSetDisplayMode(char *DisplayModeName, BOOLEAN Init);

VOID
UefiVideoPutChar(int Ch, UCHAR Attr, unsigned X, unsigned Y);

VOID
UefiVideoOutputChar(UCHAR Char, unsigned X, unsigned Y, ULONG FgColor, ULONG BgColor);

VOID
UefiPrintF(PUCHAR String, unsigned X, unsigned Y, ULONG FgColor, ULONG BgColor);

BOOLEAN
UefiMmInitializeMemoryManager(VOID);

PFREELDR_MEMORY_DESCRIPTOR
UefiMemGetMemoryMap(ULONG *MemoryMapSize);

#define ERR(fmt, ...)
#define FIXME(fmt, ...)
#define WARN(fmt, ...)
#define TRACE(fmt, ...)