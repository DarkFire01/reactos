/*
 *  FreeLoader
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#pragma once

#ifndef __MEMORY_H
#include "mm.h"
#endif

#include <drivers/xbox/xgpu.h>

#define PIC1_CONTROL_PORT      0x20
#define PIC1_DATA_PORT         0x21
#define PIC2_CONTROL_PORT      0xA0
#define PIC2_DATA_PORT         0xA1
#define PIC_EOI                0x20
#define PIC_SPECIFIC_EOI2      0x62

#define ICW1_ICW4              0x01
#define ICW1_INIT              0x10
#define ICW4_8086              0x01

#define CROMWELL_PARAM_BLOCK_OFFSET       0x00000200
#define CROMWELL_PARAM_BLOCK_SIGNATURE    'morC'
#define CROMWELL_CALLBACK_DEBUG         0
#define CROMWELL_CALLBACK_START_SERVICE 1
#define CROMWELL_CALLBACK_EXIT_SERVICE  2
#define CROMWELL_CALLBACK_CALL_IRQ      3
#define CROMWELL_CALLBACK_CHECK_KEY     4
#define CROMWELL_CALLBACK_GET_KEY       5

typedef struct {
    int Signature;
    void * BiosCallback;
    int Checksum;
} CROMWELL_PARAMETER_BLOCK;

typedef int (__fastcall *PCROMWELL_CALLBACK)(int Reason, void *Param);

extern UCHAR BitmapFont8x16[256 * 16];

VOID XboxConsPutChar(int Ch);
BOOLEAN XboxConsKbHit(VOID);
int XboxConsGetCh(VOID);

VOID XboxVideoInit(VOID);
VOID XboxVideoClearScreen(UCHAR Attr);
VIDEODISPLAYMODE XboxVideoSetDisplayMode(char *DisplayModem, BOOLEAN Init);
VOID XboxVideoGetDisplaySize(PULONG Width, PULONG Height, PULONG Depth);
ULONG XboxVideoGetBufferSize(VOID);
VOID XboxVideoGetFontsFromFirmware(PULONG RomFontPointers);
VOID XboxVideoSetTextCursorPosition(UCHAR X, UCHAR Y);
VOID XboxVideoHideShowTextCursor(BOOLEAN Show);
VOID XboxVideoPutChar(int Ch, UCHAR Attr, unsigned X, unsigned Y);
VOID XboxVideoCopyOffScreenBufferToVRAM(PVOID Buffer);
BOOLEAN XboxVideoIsPaletteFixed(VOID);
VOID XboxVideoSetPaletteColor(UCHAR Color, UCHAR Red, UCHAR Green, UCHAR Blue);
VOID XboxVideoGetPaletteColor(UCHAR Color, UCHAR* Red, UCHAR* Green, UCHAR* Blue);
VOID XboxVideoSync(VOID);
VOID XboxVideoPrepareForReactOS(VOID);
VOID XboxVideoScrollUp(VOID);
VOID XboxPrepareForReactOS(VOID);

VOID XboxMemInit(VOID);
PFREELDR_MEMORY_DESCRIPTOR XboxMemGetMemoryMap(ULONG *MemoryMapSize);

VOID XboxDiskInit(BOOLEAN Init);
BOOLEAN XboxDiskReadLogicalSectors(UCHAR DriveNumber, ULONGLONG SectorNumber, ULONG SectorCount, PVOID Buffer);
BOOLEAN XboxDiskGetDriveGeometry(UCHAR DriveNumber, PGEOMETRY DriveGeometry);
ULONG XboxDiskGetCacheableBlockCount(UCHAR DriveNumber);

TIMEINFO* XboxGetTime(VOID);

PCONFIGURATION_COMPONENT_DATA XboxHwDetect(VOID);
VOID XboxHwIdle(VOID);

VOID XboxSetLED(PCSTR Pattern);

/* EOF */
