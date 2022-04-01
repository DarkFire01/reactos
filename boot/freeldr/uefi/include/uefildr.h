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
