/*
 * PROJECT:     ReactOS GPIO framework extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Pin connect, disconnect and I/O
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The bank-facing half of the class extension. Everything here turns a request
 * expressed in controller-wide pin numbers into the per-bank calls a client
 * answers, because a client only ever sees a bank and an offset within it.
 */

#define NDEBUG
#include "gpioclxp.h"

/**
 * @brief
 * Splits a controller-wide pin number into a bank and an offset.
 *
 * @param[in] Controller
 * The controller the pin belongs to.
 *
 * @param[in] Pin
 * The pin, numbered across the whole controller.
 *
 * @param[out] BankId
 * Receives the bank it falls in.
 *
 * @param[out] Offset
 * Receives its position within that bank.
 *
 * @return
 * TRUE when the pin exists on this controller.
 */
BOOLEAN
GcxPinToBank(
    _In_ PGCX_CONTROLLER Controller,
    _In_ ULONG Pin,
    _Out_ PBANK_ID BankId,
    _Out_ PPIN_NUMBER Offset)
{
    if (Pin >= Controller->Information.TotalPins)
    {
        return FALSE;
    }

    *BankId = (BANK_ID)(Pin / Controller->Information.NumberOfPinsPerBank);
    *Offset = (PIN_NUMBER)(Pin % Controller->Information.NumberOfPinsPerBank);

    return TRUE;
}

/**
 * @brief
 * Puts a run of pins in one bank into the mode a consumer asked for.
 *
 * @param[in] Controller
 * The controller.
 *
 * @param[in] BankId
 * The bank the pins are in.
 *
 * @param[in] PinNumberTable
 * The pins, as offsets within that bank.
 *
 * @param[in] PinCount
 * How many.
 *
 * @param[in] ConnectMode
 * Input or output.
 *
 * @param[in] PullConfiguration
 * What the pad should pull to while nothing drives it.
 *
 * @return
 * STATUS_SUCCESS, or the client's failure.
 */
NTSTATUS
GcxConnectIoPins(
    _In_ PGCX_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PPIN_NUMBER PinNumberTable,
    _In_ USHORT PinCount,
    _In_ UCHAR ConnectMode,
    _In_ UCHAR PullConfiguration)
{
    GPIO_CONNECT_IO_PINS_PARAMETERS Parameters;

    if (Controller->Registration.CLIENT_ConnectIoPins == NULL)
    {
        return STATUS_NOT_SUPPORTED;
    }

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.BankId = BankId;
    Parameters.PinNumberTable = PinNumberTable;
    Parameters.PinCount = PinCount;
    Parameters.ConnectMode = ConnectMode;
    Parameters.PullConfiguration = PullConfiguration;

    return Controller->Registration.CLIENT_ConnectIoPins(
               GcxClientContext(Controller), &Parameters);
}

/**
 * @brief
 * Releases a run of pins in one bank.
 *
 * @param[in] Controller
 * The controller.
 *
 * @param[in] BankId
 * The bank the pins are in.
 *
 * @param[in] PinNumberTable
 * The pins, as offsets within that bank.
 *
 * @param[in] PinCount
 * How many.
 *
 * @return
 * STATUS_SUCCESS, or the client's failure.
 */
NTSTATUS
GcxDisconnectIoPins(
    _In_ PGCX_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PPIN_NUMBER PinNumberTable,
    _In_ USHORT PinCount)
{
    GPIO_DISCONNECT_IO_PINS_PARAMETERS Parameters;

    if (Controller->Registration.CLIENT_DisconnectIoPins == NULL)
    {
        return STATUS_NOT_SUPPORTED;
    }

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.BankId = BankId;
    Parameters.PinNumberTable = PinNumberTable;
    Parameters.PinCount = PinCount;

    return Controller->Registration.CLIENT_DisconnectIoPins(
               GcxClientContext(Controller), &Parameters);
}

/**
 * @brief
 * Reads a run of pins in one bank.
 *
 * @param[in] Controller
 * The controller.
 *
 * @param[in] BankId
 * The bank the pins are in.
 *
 * @param[in] PinNumberTable
 * The pins, as offsets within that bank.
 *
 * @param[in] PinCount
 * How many.
 *
 * @param[out] Buffer
 * Receives one bit per pin, in the order they were asked for.
 *
 * @return
 * STATUS_SUCCESS, or the client's failure.
 */
NTSTATUS
GcxReadGpioPins(
    _In_ PGCX_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PPIN_NUMBER PinNumberTable,
    _In_ ULONG PinCount,
    _Out_ PULONG64 Buffer)
{
    GPIO_READ_PINS_PARAMETERS Parameters;

    if (Controller->Registration.CLIENT_ReadGpioPins == NULL)
    {
        return STATUS_NOT_SUPPORTED;
    }

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.BankId = BankId;
    Parameters.PinNumberTable = PinNumberTable;
    Parameters.PinCount = PinCount;
    Parameters.Buffer = Buffer;

    return Controller->Registration.CLIENT_ReadGpioPins(
               GcxClientContext(Controller), &Parameters);
}

/**
 * @brief
 * Drives a run of pins in one bank.
 *
 * @param[in] Controller
 * The controller.
 *
 * @param[in] BankId
 * The bank the pins are in.
 *
 * @param[in] PinNumberTable
 * The pins, as offsets within that bank.
 *
 * @param[in] PinCount
 * How many.
 *
 * @param[in] Buffer
 * One bit per pin, in the order they are listed.
 *
 * @return
 * STATUS_SUCCESS, or the client's failure.
 */
NTSTATUS
GcxWriteGpioPins(
    _In_ PGCX_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PPIN_NUMBER PinNumberTable,
    _In_ ULONG PinCount,
    _In_ PULONG64 Buffer)
{
    GPIO_WRITE_PINS_PARAMETERS Parameters;

    if (Controller->Registration.CLIENT_WriteGpioPins == NULL)
    {
        return STATUS_NOT_SUPPORTED;
    }

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.BankId = BankId;
    Parameters.PinNumberTable = PinNumberTable;
    Parameters.PinCount = PinCount;
    Parameters.Buffer = Buffer;

    return Controller->Registration.CLIENT_WriteGpioPins(
               GcxClientContext(Controller), &Parameters);
}
