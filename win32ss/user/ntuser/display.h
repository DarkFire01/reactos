#pragma once

extern BOOL gbBaseVideo;
extern BOOL gbVideoInitialized;

NTSTATUS
NTAPI
InitVideo(VOID);

VOID
NTAPI
VideoPortCallout(
    _In_ PVOID Params);

VOID
UserVideoPortCalloutThread(
    _In_ PVOID Param);

VOID
UserSignalVideoPortCalloutReady(VOID);
