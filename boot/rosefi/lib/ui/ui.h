#pragma once

#include "../../include/rosefip.h"

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

/* Private Functions ********************************************/

VOID
RefiPrintUI(CHAR16* str, UINT32 x, UINT32 y, UINT32 Color);

VOID
RefiClearScreen(UINT32 Color);

VOID
RefiBaseDrawRandomShit(UINT32 Color);

VOID
RefiSetPixel(UINT32 x, UINT32 y, UINT32 Color);

VOID
RefiDrawRectangle(UINT32 x, UINT32 y, UINT32 width, UINT32 height, UINT32 Color);

VOID
RefiDrawUIBackground();
/* Font commands ************************************************/

VOID
RefiFontPrint(CHAR16* str, UINT32 xOff, UINT32 yOff, UINT32 Color);

BOOLEAN
RefiLoadPSF1Font();
