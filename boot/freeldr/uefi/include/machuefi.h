/* Console */

VOID
UefiInitConsole(_In_ EFI_SYSTEM_TABLE *SystemTable);

VOID
UefiConsPutChar(int Ch);

BOOLEAN
UefiConsKbHit(VOID);

int
UefiConsGetCh(void);

/* Video */

VOID
UefiInitalizeVideo(_In_ EFI_HANDLE ImageHandle,
                   _In_ EFI_SYSTEM_TABLE *SystemTable,
                   _In_ EFI_GRAPHICS_OUTPUT_PROTOCOL* gop);