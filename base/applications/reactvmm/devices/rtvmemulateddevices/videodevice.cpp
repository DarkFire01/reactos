/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The display, and the page the guest writes it into
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * The page is not the guest's memory. It is this device's, and every character
 * the guest writes arrives as an access to a window it does not have, which is
 * what a display has always been: something that remembers what was put in it
 * and shows it, rather than a corner of memory that happens to be looked at.
 *
 * Nothing is handed to whatever draws it. This says which part of the page
 * stopped being what it was, and whatever is drawing comes back for as much of
 * it as it wants, when it is ready.
 */

#include "videodevice.h"

namespace rtvm
{

/* The two that say where the cursor is, and the two that say where the page is */
#define CRTC_START_HIGH     0x0C
#define CRTC_START_LOW      0x0D
#define CRTC_CURSOR_HIGH    0x0E
#define CRTC_CURSOR_LOW     0x0F

#define VIDEO_CRTC_ADDRESS  0x03D4
#define VIDEO_CRTC_DATA     0x03D5
#define VIDEO_STATUS        0x03DA

/* The two that say how tall the screen is, the low part and the rest of it */
#define CRTC_WIDTH_END      0x01
#define CRTC_OVERFLOW       0x07
#define CRTC_PITCH          0x13
#define CRTC_HEIGHT_END     0x12
#define CRTC_DOUBLED        0x09
#define CRTC_MAXIMUM_ROW    0x09
#define CRTC_BLANK_START    0x15
#define CRTC_ROW_LENGTH     0x13

/* Which stores a write reaches, and whether the wire is split four ways */
#define SEQUENCER_STORE_MASK    0x02
#define SEQUENCER_ARRANGEMENT   0x04
#define SEQUENCER_SPLIT         0x08

/* What a write does with what it carries and with what was last read */
#define GRAPHICS_FORCED         0x00
#define GRAPHICS_FORCE_MASK     0x01
#define GRAPHICS_COMPARE        0x02
#define GRAPHICS_TURN           0x03
#define GRAPHICS_WHICH_STORE    0x04
#define GRAPHICS_MANNER         0x05
#define GRAPHICS_WINDOW         0x06
#define GRAPHICS_IGNORED        0x07
#define GRAPHICS_BITS           0x08

/* The bit in the window register that says pixels rather than characters */
#define GRAPHICS_IS_PIXELS      0x01

/* Where the one file reached through a single port keeps its last two */
#define ATTRIBUTE_MANNER        0x10
#define ATTRIBUTE_HIGH_BITS     0x14
#define ATTRIBUTE_PLANES        0x12

/* How wide a pixel's own value is before the tables get hold of it */
#define ATTRIBUTE_WHOLE_BYTE    0x40
#define ATTRIBUTE_SPLIT_INDEX   0x80

/* How often whatever is drawing is told, at most */
#define VIDEO_TELL_EVERY    40

/* WHAT IT IS *****************************************************************/

VideoDevice::VideoDevice()
{
    InitializeCriticalSection(&m_Lock);
    Reset(VDEV_STATE_NONE);
}

VideoDevice::~VideoDevice()
{
    PowerOff(VDEV_STATE_NONE);
    DeleteCriticalSection(&m_Lock);
}

STDMETHODIMP VideoDevice::QueryInterface(REFIID Interface, void **Object)
{
    if (Object == nullptr)
        return E_POINTER;

    if (IsEqualIID(Interface, IID_IUnknown) ||
        IsEqualIID(Interface, IID_IVirtualDevice))
    {
        *Object = static_cast<IVirtualDevice *>(this);
    }
    else if (IsEqualIID(Interface, IID_IVndIoPortHandler))
    {
        *Object = static_cast<IVndIoPortHandler *>(this);
    }
    else if (IsEqualIID(Interface, IID_IVndMmioHandler))
    {
        *Object = static_cast<IVndMmioHandler *>(this);
    }
    else if (IsEqualIID(Interface, IID_IVideoVdev))
    {
        *Object = static_cast<IVideoVdev *>(this);
    }
    else if (IsEqualIID(Interface, IID_IRtvmTextSurface))
    {
        *Object = static_cast<IRtvmTextSurface *>(this);
    }
    else if (IsEqualIID(Interface, IID_IRtvmPixelSurface))
    {
        *Object = static_cast<IRtvmPixelSurface *>(this);
    }
    else
    {
        *Object = nullptr;
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

STDMETHODIMP VideoDevice::GetDependencies(void *Repository, ULONG *Count,
                                          GUID **Services, ULONG *Required)
{
    static const GUID *const Wanted[] =
    {
        &IID_IVmAmd64EmulationServices,
        &IID_IRtvmVideoWatcher
    };

    UNREFERENCED_PARAMETER(Repository);

    const HRESULT Status = PublishDependencies(Wanted, ARRAYSIZE(Wanted), 1,
                                               Count, Services, Required);

    return Status;
}

STDMETHODIMP VideoDevice::StartReservingResources(void *Repository, VDEV_STATE State)
{
    UNREFERENCED_PARAMETER(Repository);
    UNREFERENCED_PARAMETER(State);

    HRESULT Status = ReservePorts(VIDEO_FIRST_PORT, VIDEO_LAST_PORT, this);

    if (SUCCEEDED(Status))
        Status = ReservePorts(VIDEO_CRTC_ADDRESS, VIDEO_CRTC_DATA, this);

    if (SUCCEEDED(Status))
        Status = ReservePorts(VIDEO_STATUS, VIDEO_STATUS, this);

    if (FAILED(Status))
        return Status;

    /* The page, which is this device's rather than anything the guest has */
    if (Emulation() == nullptr)
        return E_UNEXPECTED;

    /*
     * The whole window rather than the part the characters sit in, because
     * where inside it a guest is answered for is the guest's own choice and it
     * makes that choice by writing a register this device owns.
     */
    return ReserveMemory(VIDEO_WINDOW_BASE, VIDEO_WINDOW_SIZE, this);
}

STDMETHODIMP VideoDevice::Reset(VDEV_STATE State)
{
    UNREFERENCED_PARAMETER(State);

    EnterCriticalSection(&m_Lock);

    /* Every cell a space in the colour a screen comes up in */
    for (ULONG Index = 0; Index < VIDEO_TEXT_SIZE; Index += VIDEO_CELL_SIZE)
    {
        m_Text[Index] = ' ';
        m_Text[Index + 1] = 0x07;
    }

    memset(m_Crtc, 0, sizeof(m_Crtc));
    m_CrtcAddress = 0;

    /*
     * Where the memory is put back straight away. It is not the guest's state
     * and never was: it says where this device put the thing it draws out of,
     * and clearing it would have the firmware read back an address of nothing.
     */
    if (m_Vram != nullptr)
    {
        m_Crtc[CRTC_LINEAR_HIGH] = (UCHAR)(m_VramBase >> 24);
        m_Crtc[CRTC_LINEAR_LOW] = (UCHAR)(m_VramBase >> 16);
    }

    m_Retrace = 0;
    m_Dirty = true;

    memset(m_Store, 0, sizeof(m_Store));
    memset(m_Held, 0, sizeof(m_Held));

    memset(m_Sequencer, 0, sizeof(m_Sequencer));
    memset(m_Graphics, 0, sizeof(m_Graphics));
    memset(m_Attribute, 0, sizeof(m_Attribute));
    m_SequencerAddress = 0;
    m_GraphicsAddress = 0;
    m_AttributeAddress = 0;
    m_AttributeValue = false;
    m_Misc = 0;

    /* All four reachable, which is what a screen of characters comes up as */
    m_Sequencer[SEQUENCER_STORE_MASK] = 0x0F;
    m_Graphics[GRAPHICS_BITS] = 0xFF;
    m_Graphics[GRAPHICS_WINDOW] = 0x0E;

    /* Each of the sixteen naming itself, until the guest says otherwise */
    for (UCHAR Index = 0; Index < 16; Index++)
        m_Attribute[Index] = Index;

    memset(m_Colour, 0, sizeof(m_Colour));
    m_ColourMask = 0xFF;
    m_ColourWrite = 0;
    m_ColourRead = 0;
    m_ColourPart = 0;

    m_Changed.left = 0;
    m_Changed.top = 0;
    m_Changed.right = VIDEO_COLUMNS;
    m_Changed.bottom = VIDEO_ROWS;

    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

STDMETHODIMP VideoDevice::PowerOnCold(VDEV_STATE State)
{
    UNREFERENCED_PARAMETER(State);

    if (m_Monitor == nullptr)
        FindService(IID_IRtvmVideoWatcher, reinterpret_cast<void **>(&m_Monitor));

    /*
     * The whole of what is drawn out of, put where the guest reaches it as
     * ordinary memory. Where it goes is this device's to decide, the way a
     * board decides where a card answers, and the firmware reads it back out
     * of the two registers below rather than choosing one of its own.
     */
    if (m_Vram == nullptr)
    {
        IRtvmApertureServices *Given = nullptr;

        if (SUCCEEDED(FindService(IID_IRtvmApertureServices,
                                  reinterpret_cast<void **>(&Given))))
        {
            void *Where = nullptr;

            if (SUCCEEDED(Given->CreateAperture(VIDEO_VRAM_BASE,
                                                VIDEO_VRAM_SIZE, &Where)))
            {
                m_Vram = static_cast<UCHAR *>(Where);
                m_VramBase = VIDEO_VRAM_BASE;

                m_Crtc[CRTC_LINEAR_HIGH] = (UCHAR)(m_VramBase >> 24);
                m_Crtc[CRTC_LINEAR_LOW] = (UCHAR)(m_VramBase >> 16);
            }

            Given->Release();
        }
    }

    if (m_Thread != nullptr)
        return S_OK;

    InterlockedExchange(&m_Stopping, 0);

    m_Thread = CreateThread(nullptr, 0, Telling, this, 0, nullptr);

    return (m_Thread != nullptr) ? S_OK : E_FAIL;
}

STDMETHODIMP VideoDevice::PowerOff(VDEV_STATE State)
{
    UNREFERENCED_PARAMETER(State);

    if (m_Thread != nullptr)
    {
        InterlockedExchange(&m_Stopping, 1);
        WaitForSingleObject(m_Thread, 2000);
        CloseHandle(m_Thread);
        m_Thread = nullptr;
    }

    if (m_Monitor != nullptr)
    {
        m_Monitor->Release();
        m_Monitor = nullptr;
    }

    return S_OK;
}

/* SAYING WHAT MOVED **********************************************************/

/**
 * @brief
 * Widens what is owed to whatever is drawing so that it takes in this write.
 *
 * @remarks
 * One rectangle rather than a list of them. A guest clearing a screen writes
 * two thousand cells one at a time, and a display told two thousand times is a
 * display that spends its whole life being told.
 */
void VideoDevice::Soil(ULONG64 Address, ULONG Length)
{
    const ULONG64 Offset = Address - VIDEO_TEXT_BASE;
    const ULONG First = (ULONG)(Offset / VIDEO_CELL_SIZE);
    const ULONG Last = (ULONG)((Offset + Length - 1) / VIDEO_CELL_SIZE);

    if (First >= (VIDEO_COLUMNS * VIDEO_ROWS))
        return;

    const LONG FirstRow = (LONG)(First / VIDEO_COLUMNS);
    const LONG LastRow = (LONG)(Last / VIDEO_COLUMNS);

    if (!m_Dirty)
    {
        m_Changed.left = (LONG)(First % VIDEO_COLUMNS);
        m_Changed.top = FirstRow;
        m_Changed.right = (LONG)(Last % VIDEO_COLUMNS) + 1;
        m_Changed.bottom = LastRow + 1;
        m_Dirty = true;
        return;
    }

    if (FirstRow < m_Changed.top)
        m_Changed.top = FirstRow;

    if ((LastRow + 1) > m_Changed.bottom)
        m_Changed.bottom = LastRow + 1;

    /* Once it covers more than one row, the columns are the whole width */
    if (m_Changed.bottom > (m_Changed.top + 1))
    {
        m_Changed.left = 0;
        m_Changed.right = VIDEO_COLUMNS;
        return;
    }

    const LONG Left = (LONG)(First % VIDEO_COLUMNS);
    const LONG Right = (LONG)(Last % VIDEO_COLUMNS) + 1;

    if (Left < m_Changed.left)
        m_Changed.left = Left;

    if (Right > m_Changed.right)
        m_Changed.right = Right;
}

DWORD WINAPI VideoDevice::Telling(LPVOID Parameter)
{
    auto *Self = static_cast<VideoDevice *>(Parameter);

    while (InterlockedCompareExchange(&Self->m_Stopping, 0, 0) == 0)
    {
        Sleep(VIDEO_TELL_EVERY);

        if (InterlockedCompareExchange(&Self->m_Stopping, 0, 0) != 0)
            break;

        Self->Tell();
    }

    return 0;
}

void VideoDevice::Tell()
{
    RECT Changed;

    EnterCriticalSection(&m_Lock);

    /*
     * Nothing says a pixel moved once the guest has the memory itself: it
     * writes there and this device never hears about it. So the whole of it is
     * owed every time, which is what a display that is not told anything has
     * always had to do.
     */
    if (Enhanced() && (m_Vram != nullptr))
    {
        ULONG Width = 0;
        ULONG Height = 0;

        Shape(Width, Height);

        m_Changed.left = 0;
        m_Changed.top = 0;
        m_Changed.right = (LONG)Width;
        m_Changed.bottom = (LONG)Height;
        m_Dirty = true;
    }

    if (!m_Dirty)
    {
        LeaveCriticalSection(&m_Lock);
        return;
    }

    Changed = m_Changed;
    m_Dirty = false;

    LeaveCriticalSection(&m_Lock);

    if (m_Monitor != nullptr)
        m_Monitor->OnVideoDirt(VDEV_VIDEO_S3);
}

/* WHAT IS DRAWN **************************************************************/

STDMETHODIMP VideoDevice::IsVideoEnabled(BOOL *Enabled)
{
    if (Enabled == nullptr)
        return E_POINTER;

    /* Nothing here has a way of being turned off */
    *Enabled = TRUE;
    return S_OK;
}

STDMETHODIMP VideoDevice::Activate()
{
    return S_OK;
}

/**
 * @brief
 * How big the screen the guest arranged is.
 *
 * @remarks
 * Worked out from the registers that say where the picture ends rather than
 * from a list of the modes anything has ever asked for, because a guest is
 * free to ask for a screen that is on no such list and every one of them says
 * how big it is the same way.
 */
/*
 * Whether the guest has asked for more than a plain display can do. One bit of
 * one register says so, and nothing below it is worth reading until it does.
 */
bool VideoDevice::Enhanced() const
{
    return (m_Crtc[CRTC_MEMORY_CONFIG] & CRTC_IS_ENHANCED) != 0;
}

/**
 * @brief
 * How many bits a pixel carries.
 *
 * @remarks
 * Past sixteen colours it is the top half of one register and nothing else: a
 * pixel is a whole number of bytes and the table of colours is either in the
 * way or it is not. Below that it is worked out the way it always was, from
 * how the window is read and how many of the four stores are in use.
 */
ULONG VideoDevice::Depth() const
{
    if (Enhanced())
    {
        switch (m_Crtc[CRTC_DAC_MODE] & 0xF0)
        {
            case DAC_MODE_THIRTYTWO: return 32;
            case DAC_MODE_SIXTEEN:   return 16;
            case DAC_MODE_FIFTEEN:   return 15;
            default:                 return 8;
        }
    }

    /* A byte to a pixel, which is the one plain mode that has no planes */
    if ((m_Graphics[GRAPHICS_MANNER] & 0x40) != 0)
        return 8;

    /* Two bits to a pixel, which is the one that came from before all this */
    if ((m_Graphics[GRAPHICS_MANNER] & 0x20) != 0)
        return 2;

    /* One store rather than four, which is what one bit to a pixel means */
    if (m_Attribute[ATTRIBUTE_PLANES] == 1)
        return 1;

    return 4;
}

/*
 * How long a row is, in bytes. The register that says so counts in pairs, and
 * a display of this kind has two more places to put the rest of the number.
 */
ULONG VideoDevice::Pitch() const
{
    /*
     * The register counts in pairs and stops at a byte, so the rest of the
     * number is two bits of another one. What comes out is counted in pixels
     * rather than in bytes, which is why a row of them is only as long as this
     * once it has been told how much of a pixel there is.
     */
    ULONG Across = 2u * (m_Crtc[CRTC_ROW_LENGTH] |
                         (((ULONG)m_Crtc[CRTC_EXTENDED_SYSTEM] & 0x30) << 4));

    if ((m_Crtc[CRTC_EXTENDED_SYSTEM] & 0x30) == 0)
    {
        if ((m_Crtc[CRTC_EXTENDED_MODE] & CRTC_PITCH_PLUS_512) != 0)
            Across += 512;
    }

    if (Across == 0)
        Across = 512;

    return Across * ((Depth() + 7) / 8);
}

/* And where in the whole of it the first pixel of the first row is */
ULONG VideoDevice::Start() const
{
    ULONG Where = (ULONG)m_Crtc[CRTC_START_LOW] |
                  ((ULONG)m_Crtc[CRTC_START_HIGH] << 8);

    if (!Enhanced())
        return Where;

    if (m_Crtc[CRTC_EXTENDED_START] != 0)
    {
        Where |= (ULONG)m_Crtc[CRTC_EXTENDED_START] << 16;
    }
    else
    {
        Where |= (((ULONG)m_Crtc[CRTC_MEMORY_CONFIG] & 0x30) |
                  (((ULONG)m_Crtc[CRTC_EXTENDED_SYSTEM] & 0x03) << 6)) << 12;
    }

    return Where;
}

void VideoDevice::Shape(ULONG &Width, ULONG &Height) const
{
    /* How wide, which one more bit of another register carries the top of */
    ULONG Across = (ULONG)m_Crtc[CRTC_WIDTH_END];

    if ((m_Crtc[CRTC_EXTENDED_WIDTH] & 0x02) != 0)
        Across |= 0x100;

    Width = (Across + 1) * 8;

    /*
     * A character clock carries as many pixels as the pixels are narrow, so
     * the same register means half as many when each one is two bytes and
     * twice as many when the clock itself is being halved.
     */
    if (Depth() == 16)
        Width /= 2;
    else if ((Depth() == 2) ||
             ((Depth() == 4) && ((m_Sequencer[1] & 0x08) != 0)))
    {
        Width *= 2;
    }

    /* And how tall, whose top two bits are spread across two more */
    Height = (ULONG)m_Crtc[CRTC_HEIGHT_END]
           | (((ULONG)m_Crtc[CRTC_OVERFLOW] & 0x02) << 7)
           | (((ULONG)m_Crtc[CRTC_OVERFLOW] & 0x40) << 3);

    if ((m_Crtc[CRTC_EXTENDED_HEIGHT] & 0x02) != 0)
        Height |= 0x400;

    Height++;

    /*
     * Where the picture stops before the register that ends it says to, which
     * is how a screen shorter than the timing it is drawn with is asked for.
     */
    const ULONG Blanks = (ULONG)m_Crtc[CRTC_BLANK_START]
                       | (((ULONG)m_Crtc[CRTC_OVERFLOW] & 0x08) << 5)
                       | (((ULONG)m_Crtc[CRTC_MAXIMUM_ROW] & 0x20) << 4)
                       | (((ULONG)m_Crtc[CRTC_EXTENDED_HEIGHT] & 0x04) << 8);

    if ((Blanks + 1) < Height)
        Height = Blanks;

    /* Two passes over the screen, each drawing every other row of it */
    if ((m_Crtc[CRTC_MODE_CONTROL] & CRTC_IS_INTERLACED) != 0)
        Height *= 2;

    /* Each line drawn twice, which a short screen asks for to fill a tall one */
    if ((m_Crtc[CRTC_DOUBLED] & 0x80) != 0)
        Height /= 2;

    if ((Width == 0) || (Width > VIDEO_MAX_WIDTH))
        Width = 640;

    if ((Height <= 1) || (Height > VIDEO_MAX_HEIGHT))
        Height = 480;
}

STDMETHODIMP VideoDevice::GetSurfaceData(VDEV_SURFACE_DATA *Surface)
{
    if (Surface == nullptr)
        return E_POINTER;

    EnterCriticalSection(&m_Lock);

    if (Drawing())
    {
        ULONG Width = 0;
        ULONG Height = 0;

        Shape(Width, Height);

        const ULONG Bits = Depth();

        Surface->Width = Width;
        Surface->Height = Height;

        if (Enhanced())
        {
            /*
             * A pixel either names a colour in the last table or carries one
             * of its own, and which of the two it is is the only thing
             * whatever is drawing needs to be told apart.
             */
            Surface->Format = (Bits <= 8) ? VDEV_SURFACE_INDEXED
                                          : VDEV_SURFACE_DIRECT;
            Surface->Pitch = Pitch();
        }
        else
        {
            /* One byte to a pixel, whatever the guest arranged behind it */
            Surface->Format = VDEV_SURFACE_INDEXED;
            Surface->Pitch = Width;
        }

        LeaveCriticalSection(&m_Lock);
        return S_OK;
    }

    LeaveCriticalSection(&m_Lock);

    /* Counted in characters, because that is what this surface holds */
    Surface->Format = VDEV_SURFACE_TEXT;
    Surface->Width = VIDEO_COLUMNS;
    Surface->Height = VIDEO_ROWS;
    Surface->Pitch = VIDEO_COLUMNS * VIDEO_CELL_SIZE;

    return S_OK;
}

/**
 * @brief
 * Which of the last table's colours a pixel of a given value stands for.
 *
 * @remarks
 * A pixel's own value is not a colour. It names one of sixteen registers, and
 * that register names one of two hundred and fifty six, and only the last of
 * those says anything about red, green or blue. Where a pixel is a whole byte
 * it names the last table directly and the sixteen are not in the way at all.
 */
UCHAR VideoDevice::Colour(UCHAR Pixel) const
{
    if ((m_Attribute[ATTRIBUTE_MANNER] & ATTRIBUTE_WHOLE_BYTE) != 0)
        return Pixel;

    const UCHAR Named = m_Attribute[Pixel & 0x0F];

    if ((m_Attribute[ATTRIBUTE_MANNER] & ATTRIBUTE_SPLIT_INDEX) != 0)
    {
        return (UCHAR)((Named & 0x0F) |
                       ((m_Attribute[ATTRIBUTE_HIGH_BITS] & 0x0F) << 4));
    }

    return (UCHAR)((Named & 0x3F) |
                   ((m_Attribute[ATTRIBUTE_HIGH_BITS] & 0x0C) << 4));
}

STDMETHODIMP VideoDevice::ReadRows(ULONG First, ULONG Count, void *Rows,
                                   ULONG Length)
{
    if (Rows == nullptr)
        return E_POINTER;

    EnterCriticalSection(&m_Lock);

    if (!Drawing())
    {
        LeaveCriticalSection(&m_Lock);
        return E_UNEXPECTED;
    }

    ULONG Width = 0;
    ULONG Height = 0;

    Shape(Width, Height);

    /*
     * Past sixteen colours the pixels are not in the four stores at all: they
     * are in the memory the guest was handed, already laid out the way they
     * will be drawn, and a row of them is a copy and nothing more.
     */
    if (Enhanced() && (m_Vram != nullptr))
    {
        const ULONG Step = Pitch();

        if (((ULONG64)Count * Step) > Length)
        {
            LeaveCriticalSection(&m_Lock);
            return E_INVALIDARG;
        }

        auto *Out = static_cast<UCHAR *>(Rows);
        const ULONG From = Start() + (First * Step);

        for (ULONG Row = 0; Row < Count; Row++)
        {
            const ULONG64 At = (ULONG64)From + ((ULONG64)Row * Step);

            /* A row that runs off the end of it is left as nothing */
            if ((At + Step) <= VIDEO_VRAM_SIZE)
                memcpy(&Out[Row * Step], &m_Vram[At], Step);
            else
                memset(&Out[Row * Step], 0, Step);
        }

        LeaveCriticalSection(&m_Lock);
        return S_OK;
    }

    if (((ULONG64)Count * Width) > Length)
    {
        LeaveCriticalSection(&m_Lock);
        return E_INVALIDARG;
    }

    /*
     * How far apart two rows are, which the guest says in units of whatever it
     * decided a pixel costs. A screen where a byte is a pixel walks four times
     * as far along the stores for the same row.
     */
    const bool Whole =
        (m_Sequencer[SEQUENCER_ARRANGEMENT] & SEQUENCER_SPLIT) != 0;
    ULONG Step = (ULONG)m_Crtc[CRTC_PITCH] * 2;

    if (Whole)
        Step *= 4;

    if (Step == 0)
        Step = Whole ? Width : (Width / 8);

    auto *Out = static_cast<UCHAR *>(Rows);

    for (ULONG Row = 0; Row < Count; Row++)
    {
        const ULONG Start = (First + Row) * Step;

        for (ULONG Column = 0; Column < Width; Column++)
        {
            UCHAR Pixel;

            if (Whole)
            {
                /* One store in four holds it whole, taken in turn along */
                const ULONG Where = Start + Column;

                Pixel = m_Store[Where & 3]
                               [(Where >> 2) & (VIDEO_STORE_SIZE - 1)];
            }
            else
            {
                /* Or one bit of it from each of the four, at the same place */
                const ULONG Where = (Start + (Column >> 3)) &
                                    (VIDEO_STORE_SIZE - 1);
                const UCHAR Bit = (UCHAR)(7 - (Column & 7));

                Pixel = 0;

                for (ULONG Ply = 0; Ply < VIDEO_STORE_COUNT; Ply++)
                {
                    if (((m_Store[Ply][Where] >> Bit) & 1) != 0)
                        Pixel |= (UCHAR)(1u << Ply);
                }
            }

            Out[Row * Width + Column] = Colour(Pixel);
        }
    }

    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

STDMETHODIMP VideoDevice::ReadPalette(void *Colours, ULONG Length)
{
    if (Colours == nullptr)
        return E_POINTER;

    if (Length > sizeof(m_Colour))
        Length = sizeof(m_Colour);

    EnterCriticalSection(&m_Lock);
    memcpy(Colours, m_Colour, Length);
    LeaveCriticalSection(&m_Lock);

    return S_OK;
}

STDMETHODIMP VideoDevice::ReadCells(void *Cells, ULONG Length,
                                    ULONG *CursorColumn, ULONG *CursorRow)
{
    if ((Cells == nullptr) || (Length > VIDEO_PAGE_SIZE))
        return E_INVALIDARG;

    EnterCriticalSection(&m_Lock);

    memcpy(Cells, m_Text, Length);

    const ULONG Where = ((ULONG)m_Crtc[CRTC_CURSOR_HIGH] << 8) |
                        m_Crtc[CRTC_CURSOR_LOW];

    LeaveCriticalSection(&m_Lock);

    if (CursorColumn != nullptr)
        *CursorColumn = Where % VIDEO_COLUMNS;

    if (CursorRow != nullptr)
        *CursorRow = Where / VIDEO_COLUMNS;

    return S_OK;
}

/* THE FOUR STORES ************************************************************/

/* Whether the guest has asked for pixels rather than characters */
bool VideoDevice::Drawing() const
{
    /* Past sixteen colours there is nothing but pixels to draw */
    if (Enhanced())
        return true;

    return (m_Graphics[GRAPHICS_WINDOW] & GRAPHICS_IS_PIXELS) != 0;
}

/**
 * @brief
 * Where in the stores an address in the window lands.
 *
 * @remarks
 * How much of the window answers, and from where, is the guest's own choice
 * and it makes it by writing two bits of one register. An address outside
 * whatever it chose belongs to nothing, which is not the same as belonging to
 * this device and reading as nothing.
 */
bool VideoDevice::Window(ULONG64 Address, ULONG &Offset) const
{
    ULONG64 Base = VIDEO_WINDOW_BASE;
    ULONG64 Size = VIDEO_WINDOW_SIZE;

    switch ((m_Graphics[GRAPHICS_WINDOW] >> 2) & 3)
    {
        case 1:
            Size = 0x00010000;
            break;

        case 2:
            Base = 0x000B0000;
            Size = 0x00008000;
            break;

        case 3:
            Base = 0x000B8000;
            Size = 0x00008000;
            break;

        default:
            break;
    }

    if ((Address < Base) || (Address >= (Base + Size)))
        return false;

    Offset = (ULONG)(Address - Base);
    return true;
}

/**
 * @brief
 * One byte out of the stores, leaving behind what every store held there.
 *
 * @remarks
 * What is left behind matters more than what is handed back: a write takes
 * most of what it writes from it, so something changing part of a pixel reads
 * the byte first even when it has no use for what it says.
 */
UCHAR VideoDevice::Fetch(ULONG Offset)
{
    /* Four ways across rather than four deep, which is how a byte is a pixel */
    if ((m_Sequencer[SEQUENCER_ARRANGEMENT] & SEQUENCER_SPLIT) != 0)
    {
        const ULONG Which = Offset & 3;
        const ULONG Where = (Offset >> 2) & (VIDEO_STORE_SIZE - 1);

        for (ULONG Store = 0; Store < VIDEO_STORE_COUNT; Store++)
            m_Held[Store] = m_Store[Store][Where];

        return m_Store[Which][Where];
    }

    Offset &= (VIDEO_STORE_SIZE - 1);

    for (ULONG Store = 0; Store < VIDEO_STORE_COUNT; Store++)
        m_Held[Store] = m_Store[Store][Offset];

    /* Asking which stores agree, rather than asking one of them */
    if ((m_Graphics[GRAPHICS_MANNER] & 0x08) != 0)
    {
        const UCHAR Wanted = m_Graphics[GRAPHICS_COMPARE];
        const UCHAR Asked = m_Graphics[GRAPHICS_IGNORED];
        UCHAR Same = 0xFF;

        for (ULONG Store = 0; Store < VIDEO_STORE_COUNT; Store++)
        {
            if ((Asked & (1u << Store)) == 0)
                continue;

            Same &= ((Wanted & (1u << Store)) != 0)
                  ? m_Held[Store]
                  : (UCHAR)~m_Held[Store];
        }

        return Same;
    }

    return m_Held[m_Graphics[GRAPHICS_WHICH_STORE] & 3];
}

/* What a write does with the byte it carries and the one that was read */
static UCHAR Combine(UCHAR Carried, UCHAR Held, UCHAR Manner)
{
    switch (Manner & 3)
    {
        case 1:
            return (UCHAR)(Carried & Held);

        case 2:
            return (UCHAR)(Carried | Held);

        case 3:
            return (UCHAR)(Carried ^ Held);

        default:
            return Carried;
    }
}

/**
 * @brief
 * One byte into the stores, in whichever of the four manners is set.
 *
 * @remarks
 * Only the first of them writes what the guest actually carried. The rest are
 * there because a pixel is spread across four stores and a wire eight bits
 * wide cannot reach one of them without disturbing the others: what the guest
 * carries ends up saying which pixels to change rather than what to change
 * them to, and what to change them to comes from a register.
 */
void VideoDevice::Keep(ULONG Offset, UCHAR Value)
{
    const UCHAR Reach = (UCHAR)(m_Sequencer[SEQUENCER_STORE_MASK] & 0x0F);

    if ((m_Sequencer[SEQUENCER_ARRANGEMENT] & SEQUENCER_SPLIT) != 0)
    {
        const ULONG Which = Offset & 3;
        const ULONG Where = (Offset >> 2) & (VIDEO_STORE_SIZE - 1);

        if ((Reach & (1u << Which)) != 0)
            m_Store[Which][Where] = Value;

        return;
    }

    Offset &= (VIDEO_STORE_SIZE - 1);

    const UCHAR Manner = (UCHAR)(m_Graphics[GRAPHICS_MANNER] & 3);
    const UCHAR Turn = (UCHAR)(m_Graphics[GRAPHICS_TURN] & 7);
    const UCHAR Doing = (UCHAR)((m_Graphics[GRAPHICS_TURN] >> 3) & 3);
    const UCHAR Forced = (UCHAR)(m_Graphics[GRAPHICS_FORCED] & 0x0F);
    const UCHAR Whether = (UCHAR)(m_Graphics[GRAPHICS_FORCE_MASK] & 0x0F);
    UCHAR Bits = m_Graphics[GRAPHICS_BITS];

    UCHAR Turned = Value;

    if ((Manner == 0) || (Manner == 3))
        Turned = (UCHAR)((Value >> Turn) | (Value << (8 - Turn)));

    /* The last of them says which pixels to change and nothing else */
    if (Manner == 3)
        Bits = (UCHAR)(Bits & Turned);

    for (ULONG Store = 0; Store < VIDEO_STORE_COUNT; Store++)
    {
        if ((Reach & (1u << Store)) == 0)
            continue;

        const UCHAR Held = m_Held[Store];
        UCHAR Wanted;

        switch (Manner)
        {
            case 1:
                /* Straight back out of what the last read left behind */
                m_Store[Store][Offset] = Held;
                continue;

            case 2:
                Wanted = ((Value & (1u << Store)) != 0) ? 0xFF : 0x00;
                Wanted = Combine(Wanted, Held, Doing);
                break;

            case 3:
                Wanted = ((Forced & (1u << Store)) != 0) ? 0xFF : 0x00;
                break;

            default:
                Wanted = ((Whether & (1u << Store)) != 0)
                       ? (((Forced & (1u << Store)) != 0) ? 0xFF : 0x00)
                       : Turned;
                Wanted = Combine(Wanted, Held, Doing);
                break;
        }

        m_Store[Store][Offset] = (UCHAR)((Wanted & Bits) | (Held & ~Bits));
    }
}

/* THE PAGE, AND THE REGISTERS ************************************************/

STDMETHODIMP VideoDevice::NotifyMmioRead(ULONG64 Address, ULONG Length,
                                         void *Buffer)
{
    if (Buffer == nullptr)
        return E_INVALIDARG;

    EnterCriticalSection(&m_Lock);

    if (Drawing())
    {
        ULONG Offset = 0;
        auto *Bytes = static_cast<UCHAR *>(Buffer);

        for (ULONG Index = 0; Index < Length; Index++)
        {
            Bytes[Index] = Window(Address + Index, Offset)
                         ? Fetch(Offset)
                         : (UCHAR)0xFF;
        }

        LeaveCriticalSection(&m_Lock);
        return S_OK;
    }

    const ULONG64 Offset = Address - VIDEO_TEXT_BASE;

    if ((Address < VIDEO_TEXT_BASE) || ((Offset + Length) > VIDEO_TEXT_SIZE))
    {
        LeaveCriticalSection(&m_Lock);
        memset(Buffer, 0xFF, Length);
        return S_OK;
    }

    memcpy(Buffer, &m_Text[Offset], Length);
    LeaveCriticalSection(&m_Lock);

    return S_OK;
}

STDMETHODIMP VideoDevice::NotifyMmioWrite(ULONG64 Address, ULONG Length,
                                          const void *Buffer)
{
    if (Buffer == nullptr)
        return E_INVALIDARG;

    EnterCriticalSection(&m_Lock);

    if (Drawing())
    {
        const auto *Bytes = static_cast<const UCHAR *>(Buffer);
        ULONG Offset = 0;

        for (ULONG Index = 0; Index < Length; Index++)
        {
            if (Window(Address + Index, Offset))
                Keep(Offset, Bytes[Index]);
        }

        m_Dirty = true;
        LeaveCriticalSection(&m_Lock);
        return S_OK;
    }

    const ULONG64 Offset = Address - VIDEO_TEXT_BASE;

    if ((Address < VIDEO_TEXT_BASE) || ((Offset + Length) > VIDEO_TEXT_SIZE))
    {
        LeaveCriticalSection(&m_Lock);
        return S_OK;
    }

    memcpy(&m_Text[Offset], Buffer, Length);
    Soil(Address, Length);
    LeaveCriticalSection(&m_Lock);

    return S_OK;
}

STDMETHODIMP VideoDevice::NotifyIoPortRead(USHORT Port, USHORT Width,
                                           ULONG *Value)
{
    UNREFERENCED_PARAMETER(Width);

    if (Value == nullptr)
        return E_POINTER;

    EnterCriticalSection(&m_Lock);

    switch (Port)
    {
        case VIDEO_STATUS:
        {
            /*
             * Where the beam would be, if there were one.
             *
             * Worked out from the clock rather than made up. A bit that simply
             * changed every time it was read lets through a caller that waits
             * for the beam to come back round, and tells a caller that is
             * measuring how often that happens whatever it wants to hear: it
             * sees a frame every two reads and works out a rate of millions a
             * second, which is a number nothing knows what to do with.
             *
             * Reading this also puts the one file reached through a single
             * port back to expecting a name, which is the only way out of it
             * for a guest that has lost count of what it last wrote.
             */
            LARGE_INTEGER Now = {};
            LARGE_INTEGER Rate = {};

            QueryPerformanceCounter(&Now);
            QueryPerformanceFrequency(&Rate);

            const ULONG64 Frame =
                ((ULONG64)Rate.QuadPart / VIDEO_FRAMES_A_SECOND) + 1;
            const ULONG64 Into = (ULONG64)Now.QuadPart % Frame;
            const ULONG64 Line = (Frame / VIDEO_LINES_A_FRAME) + 1;

            UCHAR Where = 0;

            /* The last of the frame is the beam on its way back to the top */
            if ((Into / Line) >= ((VIDEO_LINES_A_FRAME * 95) / 100))
                Where |= VIDEO_IN_RETRACE;

            /* And nothing is being drawn then, or at the end of any row */
            if ((Where != 0) || ((Into % Line) >= ((Line * 4) / 5)))
                Where |= VIDEO_NOT_DRAWING;

            m_Retrace = Where;
            m_AttributeValue = false;
            *Value = Where;
            break;
        }

        case VIDEO_CRTC_DATA:
            *Value = m_Crtc[m_CrtcAddress];
            break;

        case VIDEO_CRTC_ADDRESS:
            *Value = m_CrtcAddress;
            break;

        case VIDEO_ATTRIBUTE_ADDRESS:
            *Value = m_AttributeAddress;
            break;

        case VIDEO_ATTRIBUTE_READ:
            *Value = m_Attribute[m_AttributeAddress & 0x1F];
            break;

        case VIDEO_SEQUENCER_ADDRESS:
            *Value = m_SequencerAddress;
            break;

        case VIDEO_SEQUENCER_DATA:
            *Value = m_Sequencer[m_SequencerAddress & 7];
            break;

        case VIDEO_GRAPHICS_ADDRESS:
            *Value = m_GraphicsAddress;
            break;

        case VIDEO_GRAPHICS_DATA:
            *Value = m_Graphics[m_GraphicsAddress & 0x0F];
            break;

        case VIDEO_MISC_READ:
            *Value = m_Misc;
            break;

        case VIDEO_COLOUR_MASK:
            *Value = m_ColourMask;
            break;

        case VIDEO_COLOUR_WRITE:
            *Value = m_ColourWrite;
            break;

        case VIDEO_COLOUR_DATA:
        {
            /* Three parts in turn, and the fourth read is the next colour */
            *Value = m_Colour[m_ColourRead][m_ColourPart];

            if (++m_ColourPart >= VIDEO_COLOUR_PARTS)
            {
                m_ColourPart = 0;
                m_ColourRead++;
            }

            break;
        }

        default:
            *Value = 0xFF;
            break;
    }

    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

/**
 * @brief
 * One byte into one of the registers, whichever file it belongs to.
 *
 * @remarks
 * Every register here is a byte. What arrives wider than one is two writes to
 * two ports, which is what the wire does with a wide one and what everything
 * naming a register and setting it in a single instruction relies on.
 */
STDMETHODIMP VideoDevice::NotifyIoPortWrite(USHORT Port, USHORT Width,
                                            ULONG Value)
{
    HRESULT Status = S_OK;

    for (ULONG Index = 0; Index < Width; Index++)
    {
        const HRESULT One = WriteRegister((USHORT)(Port + Index),
                                          (UCHAR)((Value >> (Index * 8)) & 0xFF));

        if (FAILED(One))
            Status = One;
    }

    return Status;
}

HRESULT VideoDevice::WriteRegister(USHORT Port, UCHAR Byte)
{
    EnterCriticalSection(&m_Lock);

    switch (Port)
    {
        case VIDEO_CRTC_ADDRESS:
            /* Every index, because the ones past the first thirty two are real */
            m_CrtcAddress = Byte;
            break;

        case VIDEO_CRTC_DATA:
            /*
             * Where the whole of what is drawn out of was put is not the
             * guest's to move. It is written down in two of these so that the
             * firmware can read it back, and a guest that writes its own
             * number over it would be told the memory is somewhere it is not.
             */
            if ((m_CrtcAddress == CRTC_LINEAR_HIGH) ||
                (m_CrtcAddress == CRTC_LINEAR_LOW))
            {
                break;
            }

            /*
             * The registers a plain display never had do not answer until the
             * two that keep them shut have been given what they are waiting
             * for. Without that, anything writing a register number it thinks
             * is a cursor position reaches one that says how much of a pixel
             * there is, and the whole screen becomes something else.
             *
             * The two that do the keeping are always open themselves, or
             * there would be no way in.
             */
            if ((m_CrtcAddress != CRTC_UNLOCK_ONE) &&
                (m_CrtcAddress != CRTC_UNLOCK_TWO))
            {
                if ((m_CrtcAddress >= CRTC_SECOND_LOCKED) &&
                    (m_Crtc[CRTC_UNLOCK_TWO] != CRTC_UNLOCK_TWO_KEY))
                {
                    break;
                }

                if ((m_CrtcAddress >= CRTC_FIRST_LOCKED) &&
                    (m_CrtcAddress < CRTC_SECOND_LOCKED) &&
                    (m_Crtc[CRTC_UNLOCK_ONE] != CRTC_UNLOCK_ONE_KEY))
                {
                    break;
                }
            }

            /*
             * And the first few are held against being written at all while
             * the bit that says so is set, which is how a mode that has been
             * set is kept from being half changed by something else.
             */
            if ((m_CrtcAddress < CRTC_PROTECTED_COUNT) &&
                ((m_Crtc[CRTC_PROTECT] & CRTC_PROTECT_HELD) != 0))
            {
                break;
            }

            m_Crtc[m_CrtcAddress] = Byte;

            /* Where the cursor is drawn is part of what is on the screen */
            if ((m_CrtcAddress == CRTC_CURSOR_HIGH) ||
                (m_CrtcAddress == CRTC_CURSOR_LOW))
            {
                m_Dirty = true;
            }

            /* And any of these is the whole of the screen becoming another */
            if ((m_CrtcAddress == CRTC_MEMORY_CONFIG) ||
                (m_CrtcAddress == CRTC_DAC_MODE) ||
                (m_CrtcAddress == CRTC_ROW_LENGTH) ||
                (m_CrtcAddress == CRTC_WIDTH_END) ||
                (m_CrtcAddress == CRTC_HEIGHT_END) ||
                (m_CrtcAddress == CRTC_OVERFLOW) ||
                (m_CrtcAddress == CRTC_EXTENDED_WIDTH) ||
                (m_CrtcAddress == CRTC_EXTENDED_HEIGHT) ||
                (m_CrtcAddress == CRTC_EXTENDED_SYSTEM) ||
                (m_CrtcAddress == CRTC_EXTENDED_MODE))
            {
                m_Dirty = true;
            }

            break;

        case VIDEO_ATTRIBUTE_ADDRESS:
            /* The name and the value in turn, through the one port */
            if (m_AttributeValue)
            {
                m_Attribute[m_AttributeAddress & 0x1F] = Byte;
                m_Dirty = true;
            }
            else
            {
                m_AttributeAddress = (UCHAR)(Byte & 0x1F);
            }

            m_AttributeValue = !m_AttributeValue;
            break;

        case VIDEO_SEQUENCER_ADDRESS:
            m_SequencerAddress = (UCHAR)(Byte & 7);
            break;

        case VIDEO_SEQUENCER_DATA:
            m_Sequencer[m_SequencerAddress & 7] = Byte;
            break;

        case VIDEO_GRAPHICS_ADDRESS:
            m_GraphicsAddress = (UCHAR)(Byte & 0x0F);
            break;

        case VIDEO_GRAPHICS_DATA:
            m_Graphics[m_GraphicsAddress & 0x0F] = Byte;

            /* Changing what the window means changes the whole screen */
            if (m_GraphicsAddress == GRAPHICS_WINDOW)
                m_Dirty = true;

            break;

        case VIDEO_MISC_WRITE:
            m_Misc = Byte;
            break;

        case VIDEO_COLOUR_MASK:
            m_ColourMask = Byte;
            break;

        case VIDEO_COLOUR_READ:
            m_ColourRead = Byte;
            m_ColourPart = 0;
            break;

        case VIDEO_COLOUR_WRITE:
            m_ColourWrite = Byte;
            m_ColourPart = 0;
            break;

        case VIDEO_COLOUR_DATA:
            /* Six bits to a part, three parts to a colour */
            m_Colour[m_ColourWrite][m_ColourPart] = (UCHAR)(Byte & 0x3F);

            if (++m_ColourPart >= VIDEO_COLOUR_PARTS)
            {
                m_ColourPart = 0;
                m_ColourWrite++;
                m_Dirty = true;
            }

            break;

        default:
            break;
    }

    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

} /* namespace rtvm */
