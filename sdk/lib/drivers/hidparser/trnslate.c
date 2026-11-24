/*
 * PROJECT:     ReactOS HID Parser Library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Usage list helpers and i8042 scan code translation
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "hidparser.h"

/*
 * Translation entries. Plain keys hold a set 1 make code, extended keys carry
 * the 0xE0 prefix in the high byte. Values from 0xF000 up select a key that
 * needs extra handling, the low byte indexes the table of that key kind.
 */
#define KEY_KIND_NAVIGATION     0xF100
#define KEY_KIND_MODIFIER       0xF200
#define KEY_KIND_LANGUAGE       0xF300
#define KEY_KIND_PRINT_SCREEN   0xF400
#define KEY_KIND_PAUSE          0xF500

#define KEY_NONE                0x0000
#define KEY_EXT(Code)           (0xE000 | (Code))
#define KEY_NAVIGATION(Index)   (KEY_KIND_NAVIGATION | (Index))
#define KEY_MODIFIER(Index)     (KEY_KIND_MODIFIER | (Index))
#define KEY_LANGUAGE(Index)     (KEY_KIND_LANGUAGE | (Index))
#define KEY_PRINT_SCREEN        KEY_KIND_PRINT_SCREEN
#define KEY_PAUSE               KEY_KIND_PAUSE
#define KEY_KIND(Entry)         ((Entry) & 0xFF00)
#define KEY_INDEX(Entry)        ((Entry) & 0x00FF)

#define SCAN_BREAK_FLAG         0x80
#define SCAN_PREFIX_E0          0xE0
#define SCAN_PREFIX_E1          0xE1
#define SCAN_LEFT_SHIFT         0x2A
#define SCAN_SYSRQ              0x54

/* Modifier state bits, see HIDP_KEYBOARD_MODIFIER_STATE */
#define STATE_LEFT_CONTROL      0x00000001
#define STATE_LEFT_SHIFT        0x00000002
#define STATE_LEFT_ALT          0x00000004
#define STATE_RIGHT_CONTROL     0x00000010
#define STATE_RIGHT_SHIFT       0x00000020
#define STATE_RIGHT_ALT         0x00000040
#define STATE_NUM_LOCK          0x00000400
#define STATE_CONTROL           (STATE_LEFT_CONTROL | STATE_RIGHT_CONTROL)
#define STATE_SHIFT             (STATE_LEFT_SHIFT | STATE_RIGHT_SHIFT)
#define STATE_ALT               (STATE_LEFT_ALT | STATE_RIGHT_ALT)

/* Lock keys toggle their state bit and track the held key 16 bits higher */
#define MODIFIER_LOCK_FIRST     8
#define MODIFIER_HELD_SHIFT     16

#define USAGE_KEYBOARD_LAST     0xFF

/* Keyboard page usages 0x00 to 0x73 */
static const USHORT HidParseKeyboardMain[] =
{
    KEY_NONE, KEY_NONE, KEY_NONE, KEY_NONE,
    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23,
    0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19,
    0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D,
    0x15, 0x2C, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x1C, 0x01, 0x0E, 0x0F,
    0x39, 0x0C, 0x0D, 0x1A, 0x1B, 0x2B, 0x2B, 0x27,
    0x28, 0x29, 0x33, 0x34, 0x35, KEY_MODIFIER(8),
    0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42,
    0x43, 0x44, 0x57, 0x58, KEY_PRINT_SCREEN, KEY_MODIFIER(9),
    KEY_PAUSE, KEY_NAVIGATION(0), KEY_NAVIGATION(1), KEY_NAVIGATION(2),
    KEY_NAVIGATION(3), KEY_NAVIGATION(4), KEY_NAVIGATION(5), KEY_NAVIGATION(6),
    KEY_NAVIGATION(7), KEY_NAVIGATION(8), KEY_NAVIGATION(9), KEY_MODIFIER(10),
    KEY_EXT(0x35), 0x37, 0x4A, 0x4E, KEY_EXT(0x1C), 0x4F, 0x50, 0x51,
    0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49, 0x52, 0x53,
    0x56, KEY_EXT(0x5D), KEY_EXT(0x5E), 0x59, 0x64, 0x65, 0x66, 0x67,
    0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x76
};

/* Keyboard page usages 0x85 to 0x94, international and language keys */
#define KEYBOARD_INTL_FIRST     0x85
static const USHORT HidParseKeyboardIntl[] =
{
    0x7E, KEY_NONE, 0x73, 0x70, 0x7D, 0x79, 0x7B, 0x5C,
    KEY_NONE, KEY_NONE, KEY_NONE, KEY_LANGUAGE(0), KEY_LANGUAGE(1), 0x78, 0x77, 0x76
};

/* Keyboard page usages 0xE0 to 0xEB, modifiers and system keys */
#define KEYBOARD_HIGH_FIRST     0xE0
static const USHORT HidParseKeyboardHigh[] =
{
    KEY_MODIFIER(0), KEY_MODIFIER(1), KEY_MODIFIER(2), KEY_MODIFIER(3),
    KEY_MODIFIER(4), KEY_MODIFIER(5), KEY_MODIFIER(6), KEY_MODIFIER(7),
    KEY_NONE, KEY_EXT(0x5E), KEY_EXT(0x5F), KEY_EXT(0x63)
};

/* Insert, Home, Page Up, Delete, End, Page Down, Right, Left, Down, Up */
static const USHORT HidParseNavigationKeys[] =
{
    KEY_EXT(0x52), KEY_EXT(0x47), KEY_EXT(0x49), KEY_EXT(0x53), KEY_EXT(0x4F),
    KEY_EXT(0x51), KEY_EXT(0x4D), KEY_EXT(0x4B), KEY_EXT(0x50), KEY_EXT(0x48)
};

/* Modifiers in HIDP_KEYBOARD_MODIFIER_STATE bit order, then Caps, Scroll and Num Lock */
static const USHORT HidParseModifierKeys[] =
{
    0x1D, 0x2A, 0x38, KEY_EXT(0x5B), KEY_EXT(0x1D), 0x36, KEY_EXT(0x38), KEY_EXT(0x5C),
    0x3A, 0x46, 0x45
};

/* Hangul and Hanja send one byte codes without break codes of their own */
static const UCHAR HidParseLanguageKeys[] =
{
    0xF2, 0xF1
};

C_ASSERT(RTL_NUMBER_OF(HidParseKeyboardMain) == 0x74);
C_ASSERT(RTL_NUMBER_OF(HidParseKeyboardIntl) == 0x95 - KEYBOARD_INTL_FIRST);
C_ASSERT(RTL_NUMBER_OF(HidParseKeyboardHigh) == 0xEC - KEYBOARD_HIGH_FIRST);

typedef struct _HIDPARSE_CONSUMER_KEY
{
    USAGE Usage;
    USHORT Entry;
} HIDPARSE_CONSUMER_KEY;

static const HIDPARSE_CONSUMER_KEY HidParseConsumerKeys[] =
{
    { 0x0224, KEY_EXT(0x6A) },  /* AC Back */
    { 0x0225, KEY_EXT(0x69) },  /* AC Forward */
    { 0x0226, KEY_EXT(0x68) },  /* AC Stop */
    { 0x0227, KEY_EXT(0x67) },  /* AC Refresh */
    { 0x0221, KEY_EXT(0x65) },  /* AC Search */
    { 0x022A, KEY_EXT(0x66) },  /* AC Bookmarks */
    { 0x0223, KEY_EXT(0x32) },  /* AC Home */
    { 0x018A, KEY_EXT(0x6C) },  /* AL Email Reader */
};

static
USHORT
HidParseKeyboardEntry(
    _In_ USAGE Usage)
{
    if (Usage < RTL_NUMBER_OF(HidParseKeyboardMain))
        return HidParseKeyboardMain[Usage];

    if (Usage >= KEYBOARD_INTL_FIRST &&
        Usage < KEYBOARD_INTL_FIRST + RTL_NUMBER_OF(HidParseKeyboardIntl))
    {
        return HidParseKeyboardIntl[Usage - KEYBOARD_INTL_FIRST];
    }

    if (Usage >= KEYBOARD_HIGH_FIRST &&
        Usage < KEYBOARD_HIGH_FIRST + RTL_NUMBER_OF(HidParseKeyboardHigh))
    {
        return HidParseKeyboardHigh[Usage - KEYBOARD_HIGH_FIRST];
    }

    return KEY_NONE;
}

static
USHORT
HidParseConsumerEntry(
    _In_ USAGE Usage)
{
    ULONG Index;

    for (Index = 0; Index < RTL_NUMBER_OF(HidParseConsumerKeys); Index++)
    {
        if (HidParseConsumerKeys[Index].Usage == Usage)
            return HidParseConsumerKeys[Index].Entry;
    }

    return KEY_NONE;
}

/**
 * @brief Sends one scan code sequence, break codes set the top bit of every byte.
 */
static
VOID
HidParseSendScanCodes(
    _In_reads_(Length) const UCHAR *Codes,
    _In_ ULONG Length,
    _In_ HIDP_KEYBOARD_DIRECTION KeyAction,
    _In_ PHIDP_INSERT_SCANCODES InsertCodesProcedure,
    _In_opt_ PVOID InsertCodesContext)
{
    UCHAR Buffer[4];
    ULONG Index;

    for (Index = 0; Index < Length; Index++)
    {
        Buffer[Index] = Codes[Index];
        if (KeyAction == HidP_Keyboard_Break)
            Buffer[Index] |= SCAN_BREAK_FLAG;
    }

    if (Length != 0)
        InsertCodesProcedure(InsertCodesContext, (PCHAR)Buffer, Length);
}

static
VOID
HidParseSendKey(
    _In_ USHORT Entry,
    _In_ HIDP_KEYBOARD_DIRECTION KeyAction,
    _In_ PHIDP_INSERT_SCANCODES InsertCodesProcedure,
    _In_opt_ PVOID InsertCodesContext)
{
    UCHAR Codes[2];

    if (Entry & 0xFF00)
    {
        Codes[0] = (UCHAR)(Entry >> 8);
        Codes[1] = (UCHAR)Entry;
        HidParseSendScanCodes(Codes, 2, KeyAction, InsertCodesProcedure, InsertCodesContext);
    }
    else
    {
        Codes[0] = (UCHAR)Entry;
        HidParseSendScanCodes(Codes, 1, KeyAction, InsertCodesProcedure, InsertCodesContext);
    }
}

static
NTSTATUS
HidParseTranslateEntry(
    _In_ USHORT Entry,
    _In_ HIDP_KEYBOARD_DIRECTION KeyAction,
    _Inout_ PHIDP_KEYBOARD_MODIFIER_STATE ModifierState,
    _In_ PHIDP_INSERT_SCANCODES InsertCodesProcedure,
    _In_opt_ PVOID InsertCodesContext)
{
    static const UCHAR PauseCodes[] = { SCAN_PREFIX_E1, 0x1D, 0x45 };
    UCHAR Codes[2];
    ULONG Index;
    ULONG Bit;

    if (Entry == KEY_NONE)
        return HIDP_STATUS_I8042_TRANS_UNKNOWN;

    switch (KEY_KIND(Entry))
    {
        case KEY_KIND_PAUSE:
        {
            /* Control and Pause is Break */
            if (ModifierState->ul & STATE_CONTROL)
            {
                HidParseSendKey(KEY_EXT(0x46), KeyAction, InsertCodesProcedure,
                                InsertCodesContext);
            }
            else
            {
                HidParseSendScanCodes(PauseCodes, (ULONG)sizeof(PauseCodes), KeyAction,
                                      InsertCodesProcedure, InsertCodesContext);
            }
            break;
        }

        case KEY_KIND_NAVIGATION:
        {
            /* With Num Lock on the keys are framed by a fake shift */
            if ((ModifierState->ul & STATE_NUM_LOCK) && KeyAction == HidP_Keyboard_Make)
            {
                HidParseSendKey(KEY_EXT(SCAN_LEFT_SHIFT), HidP_Keyboard_Make,
                                InsertCodesProcedure, InsertCodesContext);
            }

            HidParseSendKey(HidParseNavigationKeys[KEY_INDEX(Entry)], KeyAction,
                            InsertCodesProcedure, InsertCodesContext);

            if ((ModifierState->ul & STATE_NUM_LOCK) && KeyAction == HidP_Keyboard_Break)
            {
                HidParseSendKey(KEY_EXT(SCAN_LEFT_SHIFT), HidP_Keyboard_Break,
                                InsertCodesProcedure, InsertCodesContext);
            }
            break;
        }

        case KEY_KIND_MODIFIER:
        {
            Index = KEY_INDEX(Entry);
            if (Index < MODIFIER_LOCK_FIRST)
            {
                if (KeyAction == HidP_Keyboard_Make)
                    ModifierState->ul |= 1UL << Index;
                else if (KeyAction == HidP_Keyboard_Break)
                    ModifierState->ul &= ~(1UL << Index);
            }
            else
            {
                Bit = 1UL << (Index + MODIFIER_HELD_SHIFT);
                if (KeyAction == HidP_Keyboard_Break)
                {
                    ModifierState->ul &= ~Bit;
                }
                else if (KeyAction == HidP_Keyboard_Make && !(ModifierState->ul & Bit))
                {
                    /* Toggle the lock only on the first make of a held key */
                    ModifierState->ul |= Bit;
                    ModifierState->ul ^= 1UL << Index;
                }
            }

            HidParseSendKey(HidParseModifierKeys[Index], KeyAction,
                            InsertCodesProcedure, InsertCodesContext);
            break;
        }

        case KEY_KIND_LANGUAGE:
        {
            Codes[0] = SCAN_PREFIX_E1;
            Codes[1] = HidParseLanguageKeys[KEY_INDEX(Entry)];

            /* The key up event is sent as E1 followed by the make code */
            if (KeyAction == HidP_Keyboard_Make)
            {
                HidParseSendScanCodes(&Codes[1], 1, KeyAction, InsertCodesProcedure,
                                      InsertCodesContext);
            }
            else if (KeyAction == HidP_Keyboard_Break)
            {
                HidParseSendScanCodes(Codes, 2, KeyAction, InsertCodesProcedure,
                                      InsertCodesContext);
            }
            else
            {
                return HIDP_STATUS_I8042_TRANS_UNKNOWN;
            }
            break;
        }

        case KEY_KIND_PRINT_SCREEN:
        {
            if (ModifierState->ul & STATE_ALT)
            {
                HidParseSendKey(SCAN_SYSRQ, KeyAction, InsertCodesProcedure,
                                InsertCodesContext);
                break;
            }

            if (ModifierState->ul & (STATE_CONTROL | STATE_SHIFT))
            {
                HidParseSendKey(KEY_EXT(0x37), KeyAction, InsertCodesProcedure,
                                InsertCodesContext);
                break;
            }

            if (KeyAction == HidP_Keyboard_Make)
            {
                HidParseSendKey(KEY_EXT(SCAN_LEFT_SHIFT), HidP_Keyboard_Make,
                                InsertCodesProcedure, InsertCodesContext);
            }

            HidParseSendKey(KEY_EXT(0x37), KeyAction, InsertCodesProcedure,
                            InsertCodesContext);

            if (KeyAction == HidP_Keyboard_Break)
            {
                HidParseSendKey(KEY_EXT(SCAN_LEFT_SHIFT), HidP_Keyboard_Break,
                                InsertCodesProcedure, InsertCodesContext);
            }
            break;
        }

        default:
        {
            HidParseSendKey(Entry, KeyAction, InsertCodesProcedure, InsertCodesContext);
            break;
        }
    }

    return HIDP_STATUS_SUCCESS;
}

NTSTATUS
NTAPI
HidP_TranslateUsagesToI8042ScanCodes(
    _In_reads_(UsageListLength) PUSAGE ChangedUsageList,
    _In_ ULONG UsageListLength,
    _In_ HIDP_KEYBOARD_DIRECTION KeyAction,
    _Inout_ PHIDP_KEYBOARD_MODIFIER_STATE ModifierState,
    _In_ PHIDP_INSERT_SCANCODES InsertCodesProcedure,
    _In_opt_ PVOID InsertCodesContext)
{
    NTSTATUS Status = HIDP_STATUS_SUCCESS;
    ULONG Index;

    for (Index = 0; Index < UsageListLength; Index++)
    {
        if (ChangedUsageList[Index] == 0)
            break;

        if (ChangedUsageList[Index] > USAGE_KEYBOARD_LAST)
            return HIDP_STATUS_I8042_TRANS_UNKNOWN;

        Status = HidParseTranslateEntry(HidParseKeyboardEntry(ChangedUsageList[Index]),
                                        KeyAction,
                                        ModifierState,
                                        InsertCodesProcedure,
                                        InsertCodesContext);
        if (Status != HIDP_STATUS_SUCCESS)
            break;
    }

    return Status;
}

NTSTATUS
NTAPI
HidP_TranslateUsageAndPagesToI8042ScanCodes(
    _In_reads_(UsageListLength) PUSAGE_AND_PAGE ChangedUsageList,
    _In_ ULONG UsageListLength,
    _In_ HIDP_KEYBOARD_DIRECTION KeyAction,
    _Inout_ PHIDP_KEYBOARD_MODIFIER_STATE ModifierState,
    _In_ PHIDP_INSERT_SCANCODES InsertCodesProcedure,
    _In_opt_ PVOID InsertCodesContext)
{
    NTSTATUS Status = HIDP_STATUS_SUCCESS;
    USHORT Entry;
    ULONG Index;

    for (Index = 0; Index < UsageListLength; Index++)
    {
        if (ChangedUsageList[Index].Usage == 0)
            break;

        if (ChangedUsageList[Index].UsagePage == HID_USAGE_PAGE_KEYBOARD)
        {
            if (ChangedUsageList[Index].Usage > USAGE_KEYBOARD_LAST)
                return HIDP_STATUS_I8042_TRANS_UNKNOWN;

            Entry = HidParseKeyboardEntry(ChangedUsageList[Index].Usage);
        }
        else if (ChangedUsageList[Index].UsagePage == HID_USAGE_PAGE_CONSUMER)
        {
            Entry = HidParseConsumerEntry(ChangedUsageList[Index].Usage);
        }
        else
        {
            return HIDP_STATUS_I8042_TRANS_UNKNOWN;
        }

        Status = HidParseTranslateEntry(Entry,
                                        KeyAction,
                                        ModifierState,
                                        InsertCodesProcedure,
                                        InsertCodesContext);
        if (Status != HIDP_STATUS_SUCCESS)
            break;
    }

    return Status;
}

NTSTATUS
NTAPI
HidP_UsageListDifference(
    _In_reads_(UsageListLength) PUSAGE PreviousUsageList,
    _In_reads_(UsageListLength) PUSAGE CurrentUsageList,
    _Out_writes_(UsageListLength) PUSAGE BreakUsageList,
    _Out_writes_(UsageListLength) PUSAGE MakeUsageList,
    _In_ ULONG UsageListLength)
{
    ULONG BreakCount = 0;
    ULONG MakeCount = 0;
    ULONG Outer;
    ULONG Inner;

    /* Both lists end at the first zero usage */
    for (Outer = 0; Outer < UsageListLength && PreviousUsageList[Outer]; Outer++)
    {
        for (Inner = 0; Inner < UsageListLength && CurrentUsageList[Inner]; Inner++)
        {
            if (CurrentUsageList[Inner] == PreviousUsageList[Outer])
                break;
        }

        if (Inner == UsageListLength || !CurrentUsageList[Inner])
            BreakUsageList[BreakCount++] = PreviousUsageList[Outer];
    }

    for (Outer = 0; Outer < UsageListLength && CurrentUsageList[Outer]; Outer++)
    {
        for (Inner = 0; Inner < UsageListLength && PreviousUsageList[Inner]; Inner++)
        {
            if (PreviousUsageList[Inner] == CurrentUsageList[Outer])
                break;
        }

        if (Inner == UsageListLength || !PreviousUsageList[Inner])
            MakeUsageList[MakeCount++] = CurrentUsageList[Outer];
    }

    while (BreakCount < UsageListLength)
        BreakUsageList[BreakCount++] = 0;
    while (MakeCount < UsageListLength)
        MakeUsageList[MakeCount++] = 0;

    return HIDP_STATUS_SUCCESS;
}

static
BOOLEAN
HidParseIsSameUsage(
    _In_ PUSAGE_AND_PAGE First,
    _In_ PUSAGE_AND_PAGE Second)
{
    return First->Usage == Second->Usage && First->UsagePage == Second->UsagePage;
}

static
BOOLEAN
HidParseIsEmptyUsage(
    _In_ PUSAGE_AND_PAGE Entry)
{
    return Entry->Usage == 0 && Entry->UsagePage == 0;
}

NTSTATUS
NTAPI
HidP_UsageAndPageListDifference(
    _In_reads_(UsageListLength) PUSAGE_AND_PAGE PreviousUsageList,
    _In_reads_(UsageListLength) PUSAGE_AND_PAGE CurrentUsageList,
    _Out_writes_(UsageListLength) PUSAGE_AND_PAGE BreakUsageList,
    _Out_writes_(UsageListLength) PUSAGE_AND_PAGE MakeUsageList,
    _In_ ULONG UsageListLength)
{
    ULONG BreakCount = 0;
    ULONG MakeCount = 0;
    ULONG Outer;
    ULONG Inner;

    for (Outer = 0; Outer < UsageListLength; Outer++)
    {
        if (HidParseIsEmptyUsage(&PreviousUsageList[Outer]))
            break;

        for (Inner = 0; Inner < UsageListLength; Inner++)
        {
            if (HidParseIsEmptyUsage(&CurrentUsageList[Inner]) ||
                HidParseIsSameUsage(&CurrentUsageList[Inner], &PreviousUsageList[Outer]))
            {
                break;
            }
        }

        if (Inner == UsageListLength || HidParseIsEmptyUsage(&CurrentUsageList[Inner]))
            BreakUsageList[BreakCount++] = PreviousUsageList[Outer];
    }

    for (Outer = 0; Outer < UsageListLength; Outer++)
    {
        if (HidParseIsEmptyUsage(&CurrentUsageList[Outer]))
            break;

        for (Inner = 0; Inner < UsageListLength; Inner++)
        {
            if (HidParseIsEmptyUsage(&PreviousUsageList[Inner]) ||
                HidParseIsSameUsage(&PreviousUsageList[Inner], &CurrentUsageList[Outer]))
            {
                break;
            }
        }

        if (Inner == UsageListLength || HidParseIsEmptyUsage(&PreviousUsageList[Inner]))
            MakeUsageList[MakeCount++] = CurrentUsageList[Outer];
    }

    for (; BreakCount < UsageListLength; BreakCount++)
    {
        BreakUsageList[BreakCount].Usage = 0;
        BreakUsageList[BreakCount].UsagePage = 0;
    }
    for (; MakeCount < UsageListLength; MakeCount++)
    {
        MakeUsageList[MakeCount].Usage = 0;
        MakeUsageList[MakeCount].UsagePage = 0;
    }

    return HIDP_STATUS_SUCCESS;
}
