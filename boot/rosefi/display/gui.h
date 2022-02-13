#pragma once

#if 0
VOID
RefiAssignPixel(_In_ ROSEFI_FRAMEBUFFER_DATA refiFbData,
                _In_ UINT32 x, UINT32 y);

VOID
RefiDrawBox(_In_ PROSEFI_FRAMEBUFFER_DATA refiFbData,
            _In_ UINT32 x, UINT32 y,
            _In_ UINT32 width, UINT32 height,
            _In_ ULONG32 Color);
VOID
RefiClearScreenUI(_In_ ROSEFI_FRAMEBUFFER_DATA refiFbData,
                  _In_ ULONG32 Color);
#endif

VOID
RefiBaseClearScreen(UINT32 Color);

VOID
RefiBaseSetPixel(UINT32 x, UINT32 y, UINT32 Color);

VOID
RefiBaseDrawBox(UINT32 Thisx, UINT32 Thisy, UINT32 width, UINT32 height, UINT32 Color);

VOID
RefiBaseDrawRandomShit(UINT32 Color);