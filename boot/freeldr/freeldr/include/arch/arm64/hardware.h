/*
 * PROJECT:         ReactOS Boot Loader
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            boot/freeldr/freeldr/include/arch/arm/hardware.h
 * PURPOSE:         Header for ARC definitions (to be cleaned up)
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

#pragma once

#ifndef __REGISTRY_H
//#include "../../reactos/registry.h"
#endif



#define FREELDR_BASE       0x0001F000
#define FREELDR_PE_BASE    0x0001F000
#define MAX_FREELDR_PE_SIZE 0xFFFFFF

extern ULONG FirstLevelDcacheSize;
extern ULONG FirstLevelDcacheFillSize;
extern ULONG FirstLevelIcacheSize;
extern ULONG FirstLevelIcacheFillSize;
extern ULONG SecondLevelDcacheSize;
extern ULONG SecondLevelDcacheFillSize;
extern ULONG SecondLevelIcacheSize;
extern ULONG SecondLevelIcacheFillSize;

extern PVOID gDiskReadBuffer, gFileSysBuffer;
#define DiskReadBuffer ((PVOID)gDiskReadBuffer)

#define DriveMapGetBiosDriveNumber(DeviceName) 0

FORCEINLINE VOID Reboot(VOID)
{
    DbgBreakPoint();
}

#define PDE_SHIFT 20
