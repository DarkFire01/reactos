/*
 * PROJECT:     ReactOS Bluetooth probe
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Boot protocol viewer for Bluetooth keyboards and mice
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "bthprobe.h"

/* HIDP transaction headers, type in the high nibble */
#define HIDP_HANDSHAKE                      0x00
#define HIDP_CONTROL                        0x10
#define HIDP_SET_PROTOCOL                   0x70
#define HIDP_DATA                           0xA0
#define HIDP_TYPE_MASK                      0xF0

#define HIDP_REPORT_INPUT                   0x01
#define HIDP_REPORT_OUTPUT                  0x02
#define HIDP_PROTOCOL_BOOT                  0x00
#define HIDP_CONTROL_VIRTUAL_CABLE_UNPLUG   0x05

/* Boot protocol reports carry these IDs over Bluetooth */
#define HID_BOOT_KEYBOARD                   0x01
#define HID_BOOT_MOUSE                      0x02

#define HID_LED_NUM_LOCK                    0x01
#define HID_LED_CAPS_LOCK                   0x02
#define HID_LED_SCROLL_LOCK                 0x04

typedef struct _HID_SESSION
{
    BOOLEAN ProtocolSent;
    BOOLEAN ProtocolPending;
    BOOLEAN BootProtocol;
    BOOLEAN MouseLine;
    UCHAR Leds;
    USHORT Handle;
    UCHAR PreviousKeys[6];
} HID_SESSION, *PHID_SESSION;

/* Usages 0x1E to 0x27, then 0x2D to 0x38, on a US layout */
static const CHAR HidDigits[] = "1234567890";
static const CHAR HidDigitsShifted[] = "!@#$%^&*()";
static const CHAR HidSymbols[] = "-=[]\\#;'`,./";
static const CHAR HidSymbolsShifted[] = "_+{}|~:\"~<>?";

static
VOID
HidEndMouseLine(
    _Inout_ PHID_SESSION Session)
{
    if (Session->MouseLine)
    {
        printf("\n");
        Session->MouseLine = FALSE;
    }
}

static
VOID
HidSetLeds(
    _Inout_ PBT_RADIO Radio,
    _Inout_ PHID_SESSION Session)
{
    PL2CAP_CHANNEL Interrupt;
    UCHAR Report[3];

    Interrupt = L2capFindOpen(Radio, Session->Handle, L2CAP_PSM_HID_INTERRUPT);
    if (Interrupt == NULL)
        return;

    Report[0] = HIDP_DATA | HIDP_REPORT_OUTPUT;
    Report[1] = HID_BOOT_KEYBOARD;
    Report[2] = Session->Leds;
    L2capSend(Radio, Interrupt, Report, sizeof(Report));
}

static
VOID
HidTypeKey(
    _Inout_ PBT_RADIO Radio,
    _Inout_ PHID_SESSION Session,
    _In_ UCHAR Usage,
    _In_ UCHAR Modifiers)
{
    BOOLEAN Shift;
    BOOLEAN Control;
    BOOLEAN Upper;
    CHAR Letter;

    Shift = (Modifiers & 0x22) != 0;
    Control = (Modifiers & 0x11) != 0;

    HidEndMouseLine(Session);

    if (Usage >= 0x04 && Usage <= 0x1D)
    {
        Letter = (CHAR)('a' + Usage - 0x04);
        Upper = Shift != ((Session->Leds & HID_LED_CAPS_LOCK) != 0);

        if (Control)
            printf("[Ctrl+%c]", Letter - 'a' + 'A');
        else
            printf("%c", Upper ? Letter - 'a' + 'A' : Letter);
    }
    else if (Usage >= 0x1E && Usage <= 0x27)
    {
        printf("%c", Shift ? HidDigitsShifted[Usage - 0x1E] : HidDigits[Usage - 0x1E]);
    }
    else if (Usage >= 0x2D && Usage <= 0x38)
    {
        printf("%c", Shift ? HidSymbolsShifted[Usage - 0x2D] : HidSymbols[Usage - 0x2D]);
    }
    else if (Usage >= 0x3A && Usage <= 0x45)
    {
        printf("[F%u]", Usage - 0x3A + 1);
    }
    else
    {
        switch (Usage)
        {
            case 0x28:
                printf("\n");
                break;
            case 0x29:
                printf("[Esc]");
                break;
            case 0x2A:
                printf("\b \b");
                break;
            case 0x2B:
                printf("\t");
                break;
            case 0x2C:
                printf(" ");
                break;
            case 0x39:
                Session->Leds ^= HID_LED_CAPS_LOCK;
                HidSetLeds(Radio, Session);
                break;
            case 0x47:
                Session->Leds ^= HID_LED_SCROLL_LOCK;
                HidSetLeds(Radio, Session);
                break;
            case 0x53:
                Session->Leds ^= HID_LED_NUM_LOCK;
                HidSetLeds(Radio, Session);
                break;
            case 0x4C:
                printf("[Del]");
                break;
            case 0x4F:
                printf("[Right]");
                break;
            case 0x50:
                printf("[Left]");
                break;
            case 0x51:
                printf("[Down]");
                break;
            case 0x52:
                printf("[Up]");
                break;
            default:
                printf("[0x%02X]", Usage);
                break;
        }
    }

    fflush(stdout);
}

/* Modifiers, a reserved byte, then up to six keys held down */
static
VOID
HidKeyboard(
    _Inout_ PBT_RADIO Radio,
    _Inout_ PHID_SESSION Session,
    _In_reads_(8) const UCHAR *Report)
{
    ULONG Index;
    ULONG Previous;
    BOOLEAN Held;

    /* A report full of ErrorRollOver means too many keys, not new ones */
    if (Report[2] == 0x01)
        return;

    for (Index = 2; Index < 8; Index++)
    {
        if (Report[Index] < 0x04)
            continue;

        Held = FALSE;
        for (Previous = 0; Previous < 6; Previous++)
        {
            if (Session->PreviousKeys[Previous] == Report[Index])
                Held = TRUE;
        }

        if (!Held)
            HidTypeKey(Radio, Session, Report[Index], Report[0]);
    }

    CopyMemory(Session->PreviousKeys, &Report[2], sizeof(Session->PreviousKeys));
}

static
VOID
HidMouse(
    _Inout_ PHID_SESSION Session,
    _In_reads_(Length) const UCHAR *Report,
    _In_ ULONG Length)
{
    printf("\r  mouse  buttons %c%c%c  x %+4d  y %+4d  wheel %+3d  ",
           (Report[0] & 0x01) ? 'L' : '-',
           (Report[0] & 0x04) ? 'M' : '-',
           (Report[0] & 0x02) ? 'R' : '-',
           (CHAR)Report[1],
           (CHAR)Report[2],
           (Length > 3) ? (CHAR)Report[3] : 0);

    Session->MouseLine = TRUE;
    fflush(stdout);
}

static L2CAP_RECEIVE_ROUTINE HidReceiveControl;

static
VOID
HidReceiveControl(
    _Inout_ PBT_RADIO Radio,
    _In_ PL2CAP_CHANNEL Channel,
    _In_reads_(Length) const UCHAR *Data,
    _In_ ULONG Length)
{
    PHID_SESSION Session;

    UNREFERENCED_PARAMETER(Radio);

    Session = (PHID_SESSION)Channel->Context;
    if (Length < 1)
        return;

    switch (Data[0] & HIDP_TYPE_MASK)
    {
        case HIDP_HANDSHAKE:
            if (!Session->ProtocolPending)
            {
                if ((Data[0] & 0x0F) != 0)
                    printf("  HID handshake error %u\n", Data[0] & 0x0F);
                break;
            }

            Session->ProtocolPending = FALSE;
            HidEndMouseLine(Session);

            if ((Data[0] & 0x0F) == 0)
            {
                Session->BootProtocol = TRUE;
                printf("Boot protocol on. Type on the keyboard or move the mouse, Ctrl+C stops.\n");
            }
            else
            {
                printf("The device refused boot protocol, handshake %u. Showing raw reports.\n", Data[0] & 0x0F);
            }
            break;

        case HIDP_CONTROL:
            if ((Data[0] & 0x0F) == HIDP_CONTROL_VIRTUAL_CABLE_UNPLUG)
            {
                HidEndMouseLine(Session);
                printf("  The device unplugged its virtual cable, it no longer treats this host as paired\n");
            }
            break;

        default:
            break;
    }
}

static L2CAP_RECEIVE_ROUTINE HidReceiveInterrupt;

static
VOID
HidReceiveInterrupt(
    _Inout_ PBT_RADIO Radio,
    _In_ PL2CAP_CHANNEL Channel,
    _In_reads_(Length) const UCHAR *Data,
    _In_ ULONG Length)
{
    PHID_SESSION Session;
    ULONG Index;

    Session = (PHID_SESSION)Channel->Context;

    if (Length < 2 || Data[0] != (HIDP_DATA | HIDP_REPORT_INPUT))
        return;

    if (Session->BootProtocol && Data[1] == HID_BOOT_KEYBOARD && Length >= 2 + 8)
    {
        HidKeyboard(Radio, Session, &Data[2]);
        return;
    }

    if (Session->BootProtocol && Data[1] == HID_BOOT_MOUSE && Length >= 2 + 3)
    {
        HidMouse(Session, &Data[2], Length - 2);
        return;
    }

    HidEndMouseLine(Session);
    printf("  report");
    for (Index = 1; Index < Length && Index < 33; Index++)
        printf(" %02X", Data[Index]);
    printf("\n");
}

VOID
HidRun(
    _Inout_ PBT_RADIO Radio,
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Target)
{
    UCHAR LinkKey[BTH_LINK_KEY_LENGTH];
    CHAR Text[BT_ADDRESS_STRING];
    PL2CAP_CHANNEL Interrupt;
    PL2CAP_CHANNEL Control;
    HID_SESSION Session;
    PBT_LINK Link;
    BOOLEAN WasOpen;
    UCHAR Request;

    ZeroMemory(&Session, sizeof(Session));
    BtFormatAddress(Target, Text);

    if (!BtLoadKey(Radio, Target, LinkKey))
        printf("No stored pairing for %s, most keyboards will refuse. Run bthprobe pair first.\n", Text);

    SecureZeroMemory(LinkKey, sizeof(LinkKey));

    Radio->Security.UseStoredKeys = TRUE;
    Radio->Security.AcceptIncoming = TRUE;
    Radio->Security.HaveTarget = TRUE;
    CopyMemory(Radio->Security.Target, Target, BT_ADDRESS_LENGTH);

    /* A paired device often connects back on its own, so accept its channels too */
    L2capListen(Radio, L2CAP_PSM_HID_CONTROL, Target, HidReceiveControl, &Session);
    L2capListen(Radio, L2CAP_PSM_HID_INTERRUPT, Target, HidReceiveInterrupt, &Session);

    Link = BtConnect(Radio, Target);
    if (Link != NULL)
    {
        if (BtAuthenticate(Radio, Link, BT_SHORT_TIMEOUT))
            BtEncrypt(Radio, Link);

        if (Link->InUse && L2capConnect(Radio, Link, L2CAP_PSM_HID_CONTROL, HidReceiveControl, &Session) != NULL)
            L2capConnect(Radio, Link, L2CAP_PSM_HID_INTERRUPT, HidReceiveInterrupt, &Session);
    }

    Link = BtFindLinkByAddress(Radio, Target);
    if (Link == NULL || L2capFindOpen(Radio, Link->Handle, L2CAP_PSM_HID_INTERRUPT) == NULL)
        printf("Waiting for %s to connect. Press a key on it, Ctrl+C stops.\n", Text);

    WasOpen = FALSE;

    while (!BtStopRequested)
    {
        if (HciPump(Radio, 250) == HciItemError)
            break;

        Link = BtFindLinkByAddress(Radio, Target);
        Control = (Link != NULL) ? L2capFindOpen(Radio, Link->Handle, L2CAP_PSM_HID_CONTROL) : NULL;
        Interrupt = (Link != NULL) ? L2capFindOpen(Radio, Link->Handle, L2CAP_PSM_HID_INTERRUPT) : NULL;

        if (Control != NULL && Interrupt != NULL)
        {
            if (!Session.ProtocolSent)
            {
                Session.Handle = Link->Handle;
                Session.ProtocolSent = TRUE;
                Session.ProtocolPending = TRUE;
                Session.BootProtocol = FALSE;
                ZeroMemory(Session.PreviousKeys, sizeof(Session.PreviousKeys));

                printf("HID channels open, asking for boot protocol\n");

                Request = HIDP_SET_PROTOCOL | HIDP_PROTOCOL_BOOT;
                L2capSend(Radio, Control, &Request, 1);
            }

            WasOpen = TRUE;
        }
        else if (WasOpen)
        {
            WasOpen = FALSE;
            Session.ProtocolSent = FALSE;
            Session.BootProtocol = FALSE;

            HidEndMouseLine(&Session);
            printf("HID channels closed, waiting for the device to reconnect\n");
        }
    }

    HidEndMouseLine(&Session);
    printf("\nStopping\n");

    Link = BtFindLinkByAddress(Radio, Target);
    if (Link == NULL)
        return;

    /* Interrupt first, then control, as HID expects */
    Interrupt = L2capFindOpen(Radio, Link->Handle, L2CAP_PSM_HID_INTERRUPT);
    if (Interrupt != NULL)
        L2capDisconnect(Radio, Interrupt);

    Control = L2capFindOpen(Radio, Link->Handle, L2CAP_PSM_HID_CONTROL);
    if (Control != NULL)
        L2capDisconnect(Radio, Control);

    BtDisconnect(Radio, Link);
}
