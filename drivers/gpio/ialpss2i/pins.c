/*
 * PROJECT:     ReactOS Intel LPSS GPIO controller driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Pad configuration and pin I/O
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * Every one of these walks a table of pins in a single group and touches that
 * pad's PADCFG0. The reference checks HOSTSW_OWN first, because a pad the
 * firmware kept for a native function is not ours to drive.
 */

#include "gpiopriv.h"

/**
 * @brief
 * Says whether host software owns a pad, rather than the firmware.
 *
 * HOSTSW_OWN carries one bit per pad in the group; set means the pad is under
 * GPIO control. Driving a pad the firmware still owns would fight whatever
 * native function it was muxed to.
 *
 * @param[in] Controller
 * The controller.
 *
 * @param[in] BankId
 * The group.
 *
 * @param[in] PinNumber
 * The pad's index within it.
 *
 * @return
 * TRUE when the pad is ours to use.
 */
static
BOOLEAN
GpioPadIsHostOwned(
    _In_ PGPIO_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber)
{
    PULONG HostSwOwn;

    HostSwOwn = GpioGroupRegister(Controller, BankId, Controller->Layout->HostSwOwnOffset);
    if (HostSwOwn == NULL)
    {
        return FALSE;
    }

    return (BOOLEAN)((READ_REGISTER_ULONG(HostSwOwn) >> PinNumber) & 1);
}

/**
 * @brief
 * Puts a run of pads into GPIO mode, as inputs or outputs.
 *
 * @param[in] Context
 * This driver's controller storage.
 *
 * @param[in] ConnectParameters
 * The pads, and what to make them.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INVALID_PARAMETER for a pad that does not exist or
 * that the firmware still owns.
 */
NTSTATUS
NTAPI
GpioConnectIoPins(
    _In_ PVOID Context,
    _In_ PGPIO_CONNECT_IO_PINS_PARAMETERS ConnectParameters)
{
    PGPIO_CONTROLLER Controller = (PGPIO_CONTROLLER)Context;
    PGPIO_PIN_STATE PinState;
    PULONG Pad;
    ULONG Value;
    USHORT Index;
    PIN_NUMBER PinNumber;

    for (Index = 0; Index < ConnectParameters->PinCount; Index++)
    {
        PinNumber = ConnectParameters->PinNumberTable[Index];

        Pad = GpioPadAddress(Controller, ConnectParameters->BankId, PinNumber);
        if (Pad == NULL)
        {
            return STATUS_INVALID_PARAMETER;
        }

        if (!GpioPadIsHostOwned(Controller, ConnectParameters->BankId, PinNumber))
        {
            return STATUS_INVALID_PARAMETER;
        }

        Value = READ_REGISTER_ULONG(Pad);

        /* Mode 0 is GPIO; anything else is a native function */
        Value &= ~PADCFG0_PMODE_MASK;

        if (ConnectParameters->ConnectMode == ConnectModeOutput)
        {
            /* Drive it, and keep the input buffer on so it can be read back */
            Value &= ~(PADCFG0_GPIOTXDIS | PADCFG0_GPIORXDIS);
        }
        else
        {
            /* An input must not drive, or it fights whatever does */
            Value |= PADCFG0_GPIOTXDIS;
            Value &= ~PADCFG0_GPIORXDIS;
        }

        WRITE_REGISTER_ULONG(Pad, Value);

        /*
         * A pad only enters the save path once something has claimed it, so
         * the claim is recorded here rather than inferred from the registers.
         */
        PinState = GpioPinState(Controller, ConnectParameters->BankId, PinNumber);
        if (PinState != NULL)
        {
            PinState->InUse = TRUE;
        }
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Stops driving a run of pads.
 *
 * The pad is left in GPIO mode rather than put back to its native function:
 * what it was before is not recorded anywhere, and guessing would mux a pin
 * to something the firmware did not ask for.
 *
 * @param[in] Context
 * This driver's controller storage.
 *
 * @param[in] DisconnectParameters
 * The pads to release.
 *
 * @return
 * STATUS_SUCCESS.
 */
NTSTATUS
NTAPI
GpioDisconnectIoPins(
    _In_ PVOID Context,
    _In_ PGPIO_DISCONNECT_IO_PINS_PARAMETERS DisconnectParameters)
{
    PGPIO_CONTROLLER Controller = (PGPIO_CONTROLLER)Context;
    PULONG Pad;
    ULONG Value;
    USHORT Index;

    for (Index = 0; Index < DisconnectParameters->PinCount; Index++)
    {
        Pad = GpioPadAddress(Controller, DisconnectParameters->BankId,
                             DisconnectParameters->PinNumberTable[Index]);
        if (Pad == NULL)
        {
            continue;
        }

        Value = READ_REGISTER_ULONG(Pad);
        Value |= PADCFG0_GPIOTXDIS;
        WRITE_REGISTER_ULONG(Pad, Value);
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Reads a run of pads.
 *
 * @param[in] Context
 * This driver's controller storage.
 *
 * @param[in] ReadParameters
 * The pads to read, and where to put the answer.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INVALID_PARAMETER for a pad that does not exist.
 */
NTSTATUS
NTAPI
GpioReadGpioPins(
    _In_ PVOID Context,
    _In_ PGPIO_READ_PINS_PARAMETERS ReadParameters)
{
    PGPIO_CONTROLLER Controller = (PGPIO_CONTROLLER)Context;
    PULONG Pad;
    ULONG64 Result = 0;
    ULONG Index;

    for (Index = 0; Index < ReadParameters->PinCount; Index++)
    {
        Pad = GpioPadAddress(Controller, ReadParameters->BankId,
                             ReadParameters->PinNumberTable[Index]);
        if (Pad == NULL)
        {
            return STATUS_INVALID_PARAMETER;
        }

        /*
         * The answer is packed in the order the pins were asked for, not by
         * pad number: the caller gets bit 0 for its first pin.
         */
        if (READ_REGISTER_ULONG(Pad) & PADCFG0_GPIORXSTATE)
        {
            Result |= 1ULL << Index;
        }
    }

    *ReadParameters->Buffer = Result;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Drives a run of pads.
 *
 * @param[in] Context
 * This driver's controller storage.
 *
 * @param[in] WriteParameters
 * The pads to drive, and what to drive them to.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INVALID_PARAMETER for a pad that does not exist.
 */
NTSTATUS
NTAPI
GpioWriteGpioPins(
    _In_ PVOID Context,
    _In_ PGPIO_WRITE_PINS_PARAMETERS WriteParameters)
{
    PGPIO_CONTROLLER Controller = (PGPIO_CONTROLLER)Context;
    PULONG Pad;
    ULONG Value;
    ULONG64 Requested = *WriteParameters->Buffer;
    ULONG Index;

    for (Index = 0; Index < WriteParameters->PinCount; Index++)
    {
        Pad = GpioPadAddress(Controller, WriteParameters->BankId,
                             WriteParameters->PinNumberTable[Index]);
        if (Pad == NULL)
        {
            return STATUS_INVALID_PARAMETER;
        }

        Value = READ_REGISTER_ULONG(Pad);

        if ((Requested >> Index) & 1)
        {
            Value |= PADCFG0_GPIOTXSTATE;
        }
        else
        {
            Value &= ~PADCFG0_GPIOTXSTATE;
        }

        WRITE_REGISTER_ULONG(Pad, Value);
    }

    return STATUS_SUCCESS;
}
