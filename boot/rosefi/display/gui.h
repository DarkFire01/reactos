#pragma once

#include "../include/rosefip.h"

#define PSF1_MAGIC0 0x36;
#define PSF1_MAGIC1 0x04;

typedef struct PSF1_HEADER {
    UCHAR magic[2];
	UCHAR mode;
	UCHAR charsize;
} PSF1_HEADER, PPSF1_HEADER;

typedef struct PSF1_FONT {
    PPSF1_HEADER psf1_header;
    PVOID glyphBuffer;
} PSF1_FONT, *PPSF1_FONT;

VOID
RefiBaseClearScreen(UINT32 Color);

VOID
RefiBaseSetPixel(UINT32 x, UINT32 y, UINT32 Color);

VOID
RefiBaseDrawBox(UINT32 Thisx, UINT32 Thisy, UINT32 width, UINT32 height, UINT32 Color);

VOID
RefiBaseDrawRandomShit(UINT32 Color);

VOID
RosEFIAdvPutChar(PPSF1_FONT psf1_font, UINT32 colour, CHAR16 chr, UINT32 xOff, UINT32 yOff);


PSF1_HEADER 
LoadPSF1Font(EFI_SYSTEM_TABLE *SystemTable);