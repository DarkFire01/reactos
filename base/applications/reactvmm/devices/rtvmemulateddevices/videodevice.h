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

/*
 * The whole window a display of this kind can be given, and the four stores
 * behind it. A pixel is not held in one place: each store holds one bit of
 * every pixel, which is how four of them make sixteen colours out of a byte
 * that only ever carries eight pixels' worth of one.
 */
#define VIDEO_WINDOW_BASE   0x000A0000
#define VIDEO_WINDOW_SIZE   0x00020000
#define VIDEO_STORE_SIZE    0x00010000
#define VIDEO_STORE_COUNT   4

/* As much of a screen as this will describe, which is more than any mode has */
/* How fast the beam would come back round, and how many rows it draws */
#define VIDEO_FRAMES_A_SECOND   60
#define VIDEO_LINES_A_FRAME     800

/* What the register that says where it is carries */
#define VIDEO_NOT_DRAWING       0x01
#define VIDEO_IN_RETRACE        0x08

#define VIDEO_MAX_WIDTH     1600
#define VIDEO_MAX_HEIGHT    1200

/*
 * The whole of what a display of this kind draws out of, and where the guest
 * is given a window onto it.
 *
 * The four stores above are how a screen of sixteen colours is reached, a
 * byte at a time through a window that this device answers for. Anything
 * bigger than that is not reached that way at all: the guest is handed the
 * memory itself and writes pixels into it without this device hearing about
 * any of them, because a screen of a million pixels written thirty times a
 * second is not something that can be answered for one access at a time.
 */
#define VIDEO_VRAM_SIZE     0x00400000
#define VIDEO_VRAM_BASE     0xE0000000ull

/*
 * The registers a display of this kind has past the ones every display has,
 * named by what each of them decides rather than by its number.
 */
#define CRTC_MEMORY_CONFIG      0x31
#define CRTC_UNLOCK_ONE         0x38
#define CRTC_UNLOCK_TWO         0x39
#define CRTC_MODE_CONTROL       0x42
#define CRTC_EXTENDED_MODE      0x43
#define CRTC_SYSTEM_CONTROL     0x50
#define CRTC_EXTENDED_SYSTEM    0x51
#define CRTC_LINEAR_CONTROL     0x58
#define CRTC_LINEAR_HIGH        0x59
#define CRTC_LINEAR_LOW         0x5A
#define CRTC_EXTENDED_WIDTH     0x5D
#define CRTC_EXTENDED_HEIGHT    0x5E
#define CRTC_DAC_MODE           0x67
#define CRTC_EXTENDED_START     0x69

/* And what the bits of those mean */
/* Which of them are kept shut, and what opens each of the two sets */
#define CRTC_FIRST_LOCKED       0x30
#define CRTC_SECOND_LOCKED      0x40
#define CRTC_UNLOCK_ONE_KEY     0x48
#define CRTC_UNLOCK_TWO_KEY     0xA5

/* And the few at the front that are held against being written at all */
#define CRTC_PROTECT            0x11
#define CRTC_PROTECT_HELD       0x80
#define CRTC_PROTECTED_COUNT    7

#define CRTC_IS_ENHANCED        0x08
#define CRTC_IS_INTERLACED      0x20
#define CRTC_PITCH_PLUS_512     0x04
#define CRTC_LINEAR_ENABLED     0x10
#define CRTC_LINEAR_SIZE        0x03

/* How many bits a pixel carries, as the top of the one register says */
#define DAC_MODE_FIFTEEN        0x30
#define DAC_MODE_SIXTEEN        0x50
#define DAC_MODE_THIRTYTWO      0xD0

/* How many colours the last table holds, and how many parts each one has */
#define VIDEO_COLOUR_COUNT  256
#define VIDEO_COLOUR_PARTS  3

/* The register files, each reached by naming one and then reading it */
#define VIDEO_ATTRIBUTE_ADDRESS 0x03C0
#define VIDEO_ATTRIBUTE_READ    0x03C1
#define VIDEO_MISC_WRITE        0x03C2
#define VIDEO_SEQUENCER_ADDRESS 0x03C4
#define VIDEO_SEQUENCER_DATA    0x03C5
#define VIDEO_COLOUR_MASK       0x03C6
#define VIDEO_COLOUR_READ       0x03C7
#define VIDEO_COLOUR_WRITE      0x03C8
#define VIDEO_COLOUR_DATA       0x03C9
#define VIDEO_MISC_READ         0x03CC
#define VIDEO_GRAPHICS_ADDRESS  0x03CE
#define VIDEO_GRAPHICS_DATA     0x03CF

/* The whole run of them, claimed together because they are one file */
#define VIDEO_FIRST_PORT        0x03C0
#define VIDEO_LAST_PORT         0x03CF

/* How many of each there are, counting past the last one anything names */
#define VIDEO_SEQUENCER_COUNT   8
#define VIDEO_GRAPHICS_COUNT    16
#define VIDEO_ATTRIBUTE_COUNT   32

class VideoDevice : public VirtualDeviceBase,
                    public IVndIoPortHandler,
                    public IVndMmioHandler,
                    public IVideoVdev,
                    public IRtvmTextSurface,
                    public IRtvmPixelSurface
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
                                 GUID **Services, ULONG *Required) override;
    STDMETHODIMP StartReservingResources(void *Repository, VDEV_STATE State) override;
    STDMETHODIMP PowerOnCold(VDEV_STATE State) override;
    STDMETHODIMP PowerOff(VDEV_STATE State) override;
    STDMETHODIMP Reset(VDEV_STATE State) override;

    /* What whatever is drawing asks for once it has been told something moved */
    STDMETHODIMP IsVideoEnabled(BOOL *Enabled) override;
    STDMETHODIMP Activate() override;
    STDMETHODIMP GetSurfaceData(VDEV_SURFACE_DATA *Surface) override;

    /* Ours, because none of the above hands the characters over */
    STDMETHODIMP ReadCells(void *Cells, ULONG Length,
                           ULONG *CursorColumn, ULONG *CursorRow) override;

    /* Nor the pixels, when the guest has asked for those instead */
    STDMETHODIMP ReadRows(ULONG First, ULONG Count, void *Rows,
                          ULONG Length) override;
    STDMETHODIMP ReadPalette(void *Colours, ULONG Length) override;

    STDMETHODIMP NotifyUnregistered() override { return S_OK; }
    STDMETHODIMP NotifyIoPortRead(USHORT Port, USHORT Width, ULONG *Value) override;
    STDMETHODIMP NotifyIoPortWrite(USHORT Port, USHORT Width, ULONG Value) override;

    /* One byte into one register, which is all any of these ever holds */
    HRESULT WriteRegister(USHORT Port, UCHAR Byte);
    STDMETHODIMP NotifyMmioRead(ULONG64 Address, ULONG Length,
                                void *Buffer) override;
    STDMETHODIMP NotifyMmioWrite(ULONG64 Address, ULONG Length,
                                 const void *Buffer) override;

private:
    void Soil(ULONG64 Address, ULONG Length);
    static DWORD WINAPI Telling(LPVOID Parameter);
    void Tell();

    /* Whether the guest has asked for pixels rather than characters */
    bool Drawing() const;

    /* Where in the four stores an address in the window lands, or none */
    bool Window(ULONG64 Address, ULONG &Offset) const;

    UCHAR Fetch(ULONG Offset);
    void Keep(ULONG Offset, UCHAR Value);

    /* What the guest arranged, worked out from the registers that say so */
    void Shape(ULONG &Width, ULONG &Height) const;

    /*
     * Whether the guest has put the display past anything a plain one can do.
     * Everything below is only ever asked once this is true, because a plain
     * display has none of the registers they are worked out from.
     */
    bool Enhanced() const;

    /* How many bits a pixel carries, how long a row is, and where one starts */
    ULONG Depth() const;
    ULONG Pitch() const;
    ULONG Start() const;

    /* Which of the last table's colours a pixel of a given value stands for */
    UCHAR Colour(UCHAR Pixel) const;

    CRITICAL_SECTION m_Lock = {};

    /* The page itself, which is this device's and not the guest's memory */
    UCHAR m_Text[VIDEO_TEXT_SIZE] = {};

    /* And the four behind a screen made of pixels */
    UCHAR m_Store[VIDEO_STORE_COUNT][VIDEO_STORE_SIZE] = {};

    /*
     * What the last read left behind. A write takes most of what it writes
     * from these rather than from the guest, which is how a screen of four
     * stores is changed a pixel at a time through a wire eight bits wide.
     */
    UCHAR m_Held[VIDEO_STORE_COUNT] = {};

    /*
     * The whole of what is drawn out of once the guest has asked for more than
     * sixteen colours, and where the guest was given it. Handed over rather
     * than answered for, so nothing here sees a pixel being written.
     */
    UCHAR *m_Vram = nullptr;
    ULONG64 m_VramBase = 0;

    UCHAR m_CrtcAddress = 0;

    /*
     * Every index there is, because a display of this kind has registers well
     * past the twenty five a plain one stops at and they are reached the same
     * way. Masking the index down is what makes the ones above look like the
     * ones below and puts a mode number over a cursor position.
     */
    UCHAR m_Crtc[256] = {};

    UCHAR m_SequencerAddress = 0;
    UCHAR m_Sequencer[VIDEO_SEQUENCER_COUNT] = {};

    UCHAR m_GraphicsAddress = 0;
    UCHAR m_Graphics[VIDEO_GRAPHICS_COUNT] = {};

    UCHAR m_AttributeAddress = 0;
    UCHAR m_Attribute[VIDEO_ATTRIBUTE_COUNT] = {};

    /*
     * The one file reached through a single port, which takes the name and the
     * value in turn and tells them apart by which came last. Reading the one
     * that says where the beam is puts it back to expecting a name, which is
     * the only way out of it if a guest loses count.
     */
    bool m_AttributeValue = false;

    UCHAR m_Misc = 0;

    UCHAR m_Colour[VIDEO_COLOUR_COUNT][VIDEO_COLOUR_PARTS] = {};
    UCHAR m_ColourMask = 0xFF;
    UCHAR m_ColourWrite = 0;
    UCHAR m_ColourRead = 0;
    UCHAR m_ColourPart = 0;

    /* Flipped every time the status register is read, because it has to be */
    UCHAR m_Retrace = 0;

    /* What has changed since whatever is drawing was last told */
    bool m_Dirty = false;
    RECT m_Changed = {};

    HANDLE m_Thread = nullptr;
    volatile LONG m_Stopping = 0;

    /* Where what is drawn goes, which is not this device's to decide */
    IRtvmVideoWatcher *m_Monitor = nullptr;
};

} /* namespace rtvm */
