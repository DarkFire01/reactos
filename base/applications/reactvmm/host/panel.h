/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The window the machine is watched and typed at through
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include "rtvmm.h"

namespace rtvm
{

/* Widest page the panel will draw, which is more than a text mode has */
constexpr ULONG PanelColumns = 132;
constexpr ULONG PanelRows = 60;

/* And as big a screen of pixels as this will show, with its table of colours */
constexpr ULONG PanelPixelWidth = 1600;
constexpr ULONG PanelPixelHeight = 1200;
constexpr ULONG PanelColourCount = 256;

/*
 * The front panel: the guest's screen on the left, and on the right what the
 * machine is doing while it draws it.
 *
 * The other implementation of this idea shows a list of machines and a
 * thumbnail. This one shows one machine and the wires, because a machine being
 * brought up is watched rather than administered, and the question is never
 * which machine it is, it is which line just went up.
 */
class Panel : public IRtvmVideoWatcher
{
public:
    Panel() = default;
    ~Panel();

    Panel(const Panel &) = delete;
    Panel &operator=(const Panel &) = delete;

    bool Open(Machine &Subject, const char *Title);

    HWND Window() const noexcept { return m_Window; }

    /*
     * Whether the window goes away by itself once the machine has stopped. A
     * machine given a time to run was not being watched by anyone, so leaving
     * its window up would only have to be closed by hand.
     */
    void CloseWhenStopped(bool Closes) noexcept { m_CloseWhenStopped = Closes; }

    /* Where to leave a picture of the window as it last was, or nullptr */
    void CaptureTo(const char *Path);

    /* Draws and reads input until the window closes */
    void Pump();

    /*
     * Told by the display device that part of what it holds stopped being what
     * it was. Called from whichever thread that device keeps, and it fetches
     * rather than being handed anything.
     */
    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP OnVideoDirt(VDEV_VIDEO_KIND Which) override;

private:
    static LRESULT CALLBACK Dispatch(HWND Window, UINT Message,
                                     WPARAM First, LPARAM Second);
    LRESULT Handle(HWND Window, UINT Message, WPARAM First, LPARAM Second);

    void Paint(HDC Target);
    void PaintScreen(HDC Target);
    void PaintPixels(HDC Target);
    HRESULT TakePixels(IVideoVdev *Display, const VDEV_SURFACE_DATA &Surface);
    void PaintStatus(HDC Target);
    void Measure();
    void Key(WPARAM First, LPARAM Second, bool Down);

    /* How bright each lamp is now, one step dimmer every time it is drawn */
    void FadeLamps();

    /* Writes the buffer out as it stands, which is what the window is showing */
    void Capture();

    Machine *m_Machine = nullptr;
    volatile LONG m_Count = 1;
    bool m_CloseWhenStopped = false;
    Text<MAX_PATH> m_CapturePath;
    HWND m_Window = nullptr;
    HFONT m_Font = nullptr;
    HFONT m_LabelFont = nullptr;

    /* Drawn into and blitted, so that a repaint is never seen half done */
    HDC m_Buffer = nullptr;
    HBITMAP m_Surface = nullptr;
    HBITMAP m_Previous = nullptr;
    int m_BufferWidth = 0;
    int m_BufferHeight = 0;

    LONG m_CellWidth = 8;
    LONG m_CellHeight = 16;

    /*
     * The page as it was last handed up. Copied under the lock rather than
     * drawn from where the device keeps it, because the device is free to be
     * halfway through writing the next one.
     */
    CRITICAL_SECTION m_Lock = {};
    UCHAR m_Cells[PanelColumns * PanelRows * 2] = {};
    ULONG m_Columns = 80;
    ULONG m_Rows = 25;
    ULONG m_CursorColumn = 0;
    ULONG m_CursorRow = 0;
    bool m_CursorVisible = true;
    bool m_HavePage = false;

    /* The same again for a guest that has asked for pixels instead */
    /* Four bytes to a pixel, which is as much as any of them ever carries */
    UCHAR m_Pixels[PanelPixelWidth * PanelPixelHeight * 4] = {};
    UCHAR m_Palette[PanelColourCount][3] = {};
    ULONG m_PixelWidth = 0;
    ULONG m_PixelHeight = 0;

    /* How long a row of it is, and how much of one pixel of it is */
    ULONG m_PixelPitch = 0;
    ULONG m_PixelDepth = 8;
    bool m_Drawing = false;

    /* What the lamps were showing, so that only a change lights one */
    ULONG m_SeenCount[16] = {};
    UCHAR m_Lamp[16] = {};
};

} /* namespace rtvm */
