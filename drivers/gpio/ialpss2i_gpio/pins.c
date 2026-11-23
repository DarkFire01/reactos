/*
 * PROJECT:     ReactOS Intel LPSS GPIO controller driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     GpioIo connections and pin reads and writes
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ialpss2i_gpio.h"

#define NDEBUG
#include <debug.h>

_Use_decl_annotations_
NTSTATUS
NTAPI
IGpioConnectIoPins(
    PVOID Context,
    PGPIO_CONNECT_IO_PINS_PARAMETERS ConnectParameters)
{
    PIGPIO_CONTROLLER Controller = Context;
    BANK_ID BankId = ConnectParameters->BankId;
    PIN_NUMBER Pin;
    PIGPIO_PAD Pad;
    PULONG Config;
    ULONG Config0;
    ULONG Config1;
    USHORT Index;

    if (!IGpioIsBankValid(Controller, BankId))
        return STATUS_INVALID_PARAMETER;

    for (Index = 0; Index < ConnectParameters->PinCount; Index++)
    {
        Pin = ConnectParameters->PinNumberTable[Index];
        if (!IGpioIsPinValid(Controller, BankId, Pin))
            return STATUS_INVALID_PARAMETER;

        Config = IGpioPadRegister(Controller, BankId, Pin);
        Config0 = READ_REGISTER_ULONG(Config);
        Config1 = READ_REGISTER_ULONG(Config + 1);

        /* A pad muxed to a native function cannot be driven from here */
        if ((Config0 & IGPIO_CFG0_MODE_MASK) != 0)
            return STATUS_INVALID_PARAMETER;

        Pad = &Controller->Banks[BankId].Pads[Pin];
        if ((Pad->Use & (IGPIO_PAD_INPUT | IGPIO_PAD_OUTPUT)) != 0)
            return STATUS_INVALID_PARAMETER;

        Config1 = IGpioApplyPull(Config1, ConnectParameters->PullConfiguration);

        if (ConnectParameters->ConnectMode == ConnectModeInput)
        {
            Config0 = (Config0 & ~IGPIO_CFG0_RX_DISABLE) | IGPIO_CFG0_TX_DISABLE;
        }
        else if (ConnectParameters->ConnectMode == ConnectModeOutput)
        {
            /* Drive the last value written, even if that was before the connect */
            Config0 &= ~(IGPIO_CFG0_TX_DISABLE | IGPIO_CFG0_TX_STATE);
            Config0 |= IGPIO_CFG0_RX_DISABLE | (Pad->OutputValue & IGPIO_CFG0_TX_STATE);
        }

        WRITE_REGISTER_ULONG(Config + 1, Config1);
        WRITE_REGISTER_ULONG(Config, Config0);

        Pad->Use |= (ConnectParameters->ConnectMode == ConnectModeOutput) ? IGPIO_PAD_OUTPUT
                                                                          : IGPIO_PAD_INPUT;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
IGpioDisconnectIoPins(
    PVOID Context,
    PGPIO_DISCONNECT_IO_PINS_PARAMETERS DisconnectParameters)
{
    PIGPIO_CONTROLLER Controller = Context;
    BANK_ID BankId = DisconnectParameters->BankId;
    BOOLEAN Preserve;
    PIN_NUMBER Pin;
    PIGPIO_PAD Pad;
    PULONG Config;
    ULONG Config0;
    ULONG Index;

    if (!IGpioIsBankValid(Controller, BankId))
        return STATUS_INVALID_PARAMETER;

    Preserve = DisconnectParameters->DisconnectFlags.PreserveConfiguration ? TRUE : FALSE;

    for (Index = 0; Index < DisconnectParameters->PinCount; Index++)
    {
        Pin = DisconnectParameters->PinNumberTable[Index];
        if (!IGpioIsPinValid(Controller, BankId, Pin))
            return STATUS_INVALID_PARAMETER;

        Config = IGpioPadRegister(Controller, BankId, Pin);
        Config0 = READ_REGISTER_ULONG(Config);
        if ((Config0 & IGPIO_CFG0_MODE_MASK) != 0)
            return STATUS_INVALID_PARAMETER;

        Pad = &Controller->Banks[BankId].Pads[Pin];

        switch (DisconnectParameters->DisconnectMode)
        {
            case ConnectModeInput:
                /* The receiver stays on for a pad that still interrupts or routes to the APIC */
                if (!Preserve &&
                    (Pad->Use & IGPIO_PAD_INTERRUPT) == 0 &&
                    (Config0 & IGPIO_CFG0_ROUTE_IOAPIC) == 0)
                {
                    Config0 |= IGPIO_CFG0_RX_DISABLE;
                }
                break;

            case ConnectModeOutput:
                if (!Preserve)
                    Config0 |= IGPIO_CFG0_TX_DISABLE;
                break;

            default:
                Config0 |= IGPIO_CFG0_TX_DISABLE | IGPIO_CFG0_RX_DISABLE;
                break;
        }

        WRITE_REGISTER_ULONG(Config, Config0);
        Pad->Use &= ~(IGPIO_PAD_INPUT | IGPIO_PAD_OUTPUT);
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Reads the receive state of each pin into consecutive bits of the buffer.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
IGpioReadGpioPins(
    PVOID Context,
    PGPIO_READ_PINS_PARAMETERS ReadParameters)
{
    PIGPIO_CONTROLLER Controller = Context;
    BANK_ID BankId = ReadParameters->BankId;
    PUCHAR Bits = ReadParameters->Buffer;
    PIN_NUMBER Pin;
    ULONG Config0;
    ULONG Index;

    if (!IGpioIsBankValid(Controller, BankId))
        return STATUS_INVALID_PARAMETER;

    for (Index = 0; Index < ReadParameters->PinCount; Index++)
    {
        Pin = ReadParameters->PinNumberTable[Index];
        if (!IGpioIsPinValid(Controller, BankId, Pin))
            return STATUS_INVALID_PARAMETER;

        Config0 = READ_REGISTER_ULONG(IGpioPadRegister(Controller, BankId, Pin));
        if ((Config0 & IGPIO_CFG0_MODE_MASK) != 0)
            return STATUS_INVALID_PARAMETER;

        if ((Controller->Banks[BankId].Pads[Pin].Use & (IGPIO_PAD_INPUT | IGPIO_PAD_OUTPUT)) == 0)
            return STATUS_INVALID_PARAMETER;

        if ((Index % 8) == 0)
            Bits[Index / 8] = 0;

        if ((Config0 & IGPIO_CFG0_RX_STATE) != 0)
            Bits[Index / 8] |= (UCHAR)(1 << (Index % 8));
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Latches the value for each pin and drives it on the pins connected as outputs.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
IGpioWriteGpioPins(
    PVOID Context,
    PGPIO_WRITE_PINS_PARAMETERS WriteParameters)
{
    PIGPIO_CONTROLLER Controller = Context;
    BANK_ID BankId = WriteParameters->BankId;
    PUCHAR Bits = WriteParameters->Buffer;
    PIN_NUMBER Pin;
    PIGPIO_PAD Pad;
    PULONG Config;
    ULONG Config0;
    ULONG Index;
    UCHAR Value;

    if (!IGpioIsBankValid(Controller, BankId))
        return STATUS_SUCCESS;

    for (Index = 0; Index < WriteParameters->PinCount; Index++)
    {
        Pin = WriteParameters->PinNumberTable[Index];
        if (!IGpioIsPinValid(Controller, BankId, Pin))
            break;

        Config = IGpioPadRegister(Controller, BankId, Pin);
        Config0 = READ_REGISTER_ULONG(Config);
        if ((Config0 & IGPIO_CFG0_MODE_MASK) != 0)
            break;

        Value = (Bits[Index / 8] >> (Index % 8)) & 1;
        Pad = &Controller->Banks[BankId].Pads[Pin];
        Pad->OutputValue = Value;

        if ((Pad->Use & IGPIO_PAD_OUTPUT) == 0)
            continue;

        WRITE_REGISTER_ULONG(Config, (Config0 & ~IGPIO_CFG0_TX_STATE) | Value);
    }

    return STATUS_SUCCESS;
}
