/*
 * PROJECT:     ReactOS Webcam Viewer
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Header file for webcam viewer application
 * COPYRIGHT:   Copyright 2026 ReactOS Contributors
 */

#ifndef _WEBCAMVIEWER_H_
#define _WEBCAMVIEWER_H_

#include <windows.h>

/* Application state structure */
typedef struct {
    HWND hwndMain;              /* Main window handle */
    HWND hwndStatus;            /* Status bar window handle */
    HWND hwndList;              /* Device list window handle */
    HANDLE hKsDeviceHandle;     /* KS device handle */
    HANDLE hKsPin;              /* KS pin handle for streaming */
    BOOL bIsStreaming;          /* Streaming state */
    DWORD dwFrameCount;         /* Number of frames received */
    DWORD dwDroppedFrames;      /* Number of dropped frames */
} APP_STATE;

/* Function prototypes */
BOOL EnumerateWebcams(HWND hwndList);
BOOL OpenWebcamPin(HWND hwndList);
VOID UpdateStatus(const char *format, ...);
VOID ListDevices(HWND hwndList);

#endif /* _WEBCAMVIEWER_H_ */
