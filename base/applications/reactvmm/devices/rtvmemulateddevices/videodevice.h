/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The display, and the page the guest writes it into
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include "vdevbase.h"

#include <string.h>

namespace rtvm
{

/* Where the page lives, and how much of it there is */
#define VIDEO_TEXT_BASE     0x000B8000
#define VIDEO_TEXT_SIZE     0x00008000

#define VIDEO_COLUMNS       80
#define VIDEO_ROWS          25
#define VIDEO_CELL_SIZE     2
#define VIDEO_PAGE_SIZE     (VIDEO_COLUMNS * VIDEO_ROWS * VIDEO_CELL_SIZE)

class VideoDevice : public VirtualDeviceBase,
                    public IVndIoPortHandler,
                    public IVndMmioHandler,
                    public IVideoVdev,
                    public IRtvmTextSurface
{
public:
    VideoDevice();
    ~VideoDevice() override;

    VideoDevice(const VideoDevice &) = delete;
    VideoDevice &operator=(const VideoDevice &) = delete;

    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override;
    STDMETHODIMP_(ULONG) AddRef() override { return VirtualDeviceBase::AddRef(); }
    STDMETHODIMP_(ULONG) Release() override { return VirtualDeviceBase::Release(); }

    STDMETHODIMP GetDependencies(void *Repository, ULONG *Count,
                                 GUID **Services, ULONG *Optional) override;
    STDMETHODIMP StartReservingResources() override;
    STDMETHODIMP PowerOnCold() override;
    STDMETHODIMP PowerOff() override;
    STDMETHODIMP Reset() override;

    /* What whatever is drawing asks for once it has been told something moved */
    STDMETHODIMP IsVideoEnabled(BOOL *Enabled) override;
    STDMETHODIMP Activate() override;
    STDMETHODIMP GetSurfaceData(VDEV_SURFACE_DATA *Surface) override;

    /* Ours, because none of the above hands the characters over */
    STDMETHODIMP ReadCells(void *Cells, ULONG Length,
                           ULONG *CursorColumn, ULONG *CursorRow) override;

    STDMETHODIMP NotifyUnregistered() override { return S_OK; }
    STDMETHODIMP NotifyIoPortRead(USHORT Port, ULONG Width, ULONG *Value) override;
    STDMETHODIMP NotifyIoPortWrite(USHORT Port, ULONG Width, ULONG Value) override;
    STDMETHODIMP NotifyMmioRead(ULONG64 Address, ULONG Length,
                                void *Buffer) override;
    STDMETHODIMP NotifyMmioWrite(ULONG64 Address, ULONG Length,
                                 const void *Buffer) override;

private:
    void Soil(ULONG64 Address, ULONG Length);
    static DWORD WINAPI Telling(LPVOID Parameter);
    void Tell();

    CRITICAL_SECTION m_Lock = {};

    /* The page itself, which is this device's and not the guest's memory */
    UCHAR m_Text[VIDEO_TEXT_SIZE] = {};

    UCHAR m_CrtcAddress = 0;
    UCHAR m_Crtc[32] = {};

    /* Flipped every time the status register is read, because it has to be */
    UCHAR m_Retrace = 0;

    /* What has changed since whatever is drawing was last told */
    bool m_Dirty = false;
    RECT m_Changed = {};

    HANDLE m_Thread = nullptr;
    volatile LONG m_Stopping = 0;

    /* Where what is drawn goes, which is not this device's to decide */
    IMonitorDevice *m_Monitor = nullptr;
};

} /* namespace rtvm */
