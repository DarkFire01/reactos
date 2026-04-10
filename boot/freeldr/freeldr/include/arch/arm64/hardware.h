/*
 * PROJECT:         ReactOS Boot Loader
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            boot/freeldr/freeldr/include/arch/arm64/hardware.h
 * PURPOSE:         ARM64 boot hardware declarations for FreeLoader
 */

#pragma once

#define PtrToPfn(p) ((((ULONG64)(p)) >> PAGE_SHIFT) & ((1ULL << MM_PAGE_FRAME_NUMBER_SIZE) - 1))

#define VAtoPXI(va) ((((ULONG64)(va)) >> PXI_SHIFT) & 0x1FF)
#define VAtoPPI(va) ((((ULONG64)(va)) >> PPI_SHIFT) & 0x1FF)
#define VAtoPDI(va) ((((ULONG64)(va)) >> PDI_SHIFT) & 0x1FF)
#define VAtoPTI(va) ((((ULONG64)(va)) >> PTI_SHIFT) & 0x1FF)

extern UCHAR FrldrBootDrive;
extern ULONG FrldrBootPartition;

UCHAR
DriveMapGetBiosDriveNumber(
    _In_ PCSTR DeviceName);

VOID
StallExecutionProcessor(
    _In_ ULONG Microseconds);

DECLSPEC_NORETURN
VOID
__cdecl
Reboot(
    VOID);
