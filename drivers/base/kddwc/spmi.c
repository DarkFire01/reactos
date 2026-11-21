/*
 * PROJECT:     ReactOS DWC3 Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PMIC register access through a version 7 SPMI arbiter, and the eUSB2 repeater behind it
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kddwc.h"

/* Arbiter core registers */
#define SPMI_ARB_VERSION                0x0000
#define SPMI_ARB_VERSION_MAJOR(Value)   ((Value) >> 28)
#define SPMI_ARB_FEATURES(Bus)          (0x0004 + (Bus) * 4)
#define SPMI_ARB_FEATURES_APIDS(Value)  ((Value) & 0x7FF)
#define SPMI_ARB_APID_MAP(Apid)         (0x2000 + (Apid) * 4)
#define SPMI_ARB_APID_MAP_END           0x3000
#define SPMI_ARB_APID_MAP_PPID(Value)   (((Value) >> 8) & 0xFFF)
#define SPMI_ARB_OWNER(Value)           ((Value) & 0x7)

/* Registers of one channel */
#define SPMI_CHANNEL_COMMAND            0x00
#define SPMI_CHANNEL_STATUS             0x08
#define SPMI_CHANNEL_WRITE_DATA         0x10
#define SPMI_CHANNEL_READ_DATA          0x18
#define SPMI_STATUS_DONE                0x01
#define SPMI_STATUS_ERRORS              0x0E

/* Single byte extended register commands */
#define SPMI_OPCODE_WRITE               0
#define SPMI_OPCODE_READ                1
#define SPMI_COMMAND(Opcode, Address)   (((ULONG)(Opcode) << 27) | (((ULONG)(Address) & 0xFF) << 4))

/* Write channels are one per APID, observer channels one per APID within each execution environment */
#define SPMI_CHANNEL_STRIDE             0x1000
#define SPMI_OBSERVER_STRIDE            0x20
#define SPMI_OBSERVER_EE_STRIDE         0x8000

/* The application processor's execution environment */
#define SPMI_APPS_EE                    0

#define SPMI_TIMEOUT_US                 100

/* eUSB2 repeater registers, offsets inside its peripheral */
#define EUSB2_STATUS                    0x08
#define EUSB2_STATUS_READY              0x80
#define EUSB2_ENABLE                    0x46
#define EUSB2_ENABLE_REPEATER           0x80
#define EUSB2_FORCE_ENABLE_5            0xE8
#define EUSB2_FORCE_VALUE_5             0xED

#define EUSB2_READY_TIMEOUT_US          1000

/**
 * @brief Finds the arbiter channel of a peripheral on one bus.
 *
 * APIDs of the first bus come first, the second bus numbers its own after
 * them. Writing needs a channel the application processor owns.
 */
static
BOOLEAN
KdDwcSpmiFindChannel(
    _In_ const KDDWC_SPMI* Spmi,
    _In_ ULONG Bus,
    _In_ ULONG Ppid,
    _In_ BOOLEAN ForWrite,
    _Out_ PULONG Apid)
{
    PUCHAR Core = KdDwcMapRegister(Spmi->CoreBase);
    PUCHAR Register;
    ULONG First = 0, Count = 0;
    ULONG i;

    if (Core == NULL || Bus >= RTL_NUMBER_OF(Spmi->BusConfigBase))
        return FALSE;

    if (SPMI_ARB_VERSION_MAJOR(KdDwcRead(Core, SPMI_ARB_VERSION)) != 7)
    {
        KdDwcTrace("SPMI arbiter version 0x%lx is not 7\n", KdDwcRead(Core, SPMI_ARB_VERSION));
        return FALSE;
    }

    for (i = 0; i <= Bus; i++)
    {
        First += Count;
        Count = SPMI_ARB_FEATURES_APIDS(KdDwcRead(Core, SPMI_ARB_FEATURES(i)));
    }

    for (i = First; i < First + Count && SPMI_ARB_APID_MAP(i) < SPMI_ARB_APID_MAP_END; i++)
    {
        Register = KdDwcMapRegister(Spmi->CoreBase + SPMI_ARB_APID_MAP(i));
        if (Register == NULL)
            return FALSE;

        if (KdDwcRead(Register, 0) == 0 || SPMI_ARB_APID_MAP_PPID(KdDwcRead(Register, 0)) != Ppid)
            continue;

        if (ForWrite)
        {
            Register = KdDwcMapRegister(Spmi->BusConfigBase[Bus] + (i - First) * sizeof(ULONG));
            if (Register == NULL || SPMI_ARB_OWNER(KdDwcRead(Register, 0)) != SPMI_APPS_EE)
                continue;
        }

        *Apid = i;
        return TRUE;
    }

    KdDwcTrace("no SPMI channel for bus %lu PPID 0x%lx\n", Bus, Ppid);
    return FALSE;
}

/* Starts a command on a channel and waits for the arbiter to finish it */
static
BOOLEAN
KdDwcSpmiRun(
    _In_ PUCHAR Channel,
    _In_ ULONG Command)
{
    ULONG Status;
    ULONG Waited;

    KdDwcWrite(Channel, SPMI_CHANNEL_COMMAND, Command);
    for (Waited = 0; Waited < SPMI_TIMEOUT_US; Waited++)
    {
        Status = KdDwcRead(Channel, SPMI_CHANNEL_STATUS);
        if (Status & SPMI_STATUS_DONE)
        {
            if (Status & SPMI_STATUS_ERRORS)
            {
                KdDwcTrace("SPMI command 0x%lx failed, status 0x%lx\n", Command, Status);
                return FALSE;
            }

            return TRUE;
        }

        KdDwcStall(1);
    }

    KdDwcTrace("SPMI command 0x%lx timed out\n", Command);
    return FALSE;
}

/**
 * @brief Writes one register of a PMIC peripheral.
 *
 * @param[in] Bus
 * The arbiter bus the PMIC is on.
 *
 * @param[in] Sid
 * The PMIC's slave ID.
 *
 * @param[in] Address
 * Peripheral in the high byte, register in the low byte.
 */
BOOLEAN
NTAPI
KdDwcSpmiWrite(
    _In_ const KDDWC_SPMI* Spmi,
    _In_ ULONG Bus,
    _In_ UCHAR Sid,
    _In_ USHORT Address,
    _In_ UCHAR Value)
{
    PUCHAR Channel;
    ULONG Apid;

    if (!KdDwcSpmiFindChannel(Spmi, Bus, ((ULONG)Sid << 8) | (Address >> 8), TRUE, &Apid))
        return FALSE;

    Channel = KdDwcMapRegister(Spmi->ChannelBase + (ULONG64)Apid * SPMI_CHANNEL_STRIDE);
    if (Channel == NULL)
        return FALSE;

    KdDwcWrite(Channel, SPMI_CHANNEL_WRITE_DATA, Value);
    return KdDwcSpmiRun(Channel, SPMI_COMMAND(SPMI_OPCODE_WRITE, Address));
}

/**
 * @brief Reads one register of a PMIC peripheral through the application processor's observer channel.
 */
BOOLEAN
NTAPI
KdDwcSpmiRead(
    _In_ const KDDWC_SPMI* Spmi,
    _In_ ULONG Bus,
    _In_ UCHAR Sid,
    _In_ USHORT Address,
    _Out_ PUCHAR Value)
{
    PUCHAR Channel;
    ULONG Apid;

    *Value = 0;
    if (!KdDwcSpmiFindChannel(Spmi, Bus, ((ULONG)Sid << 8) | (Address >> 8), FALSE, &Apid))
        return FALSE;

    Channel = KdDwcMapRegister(Spmi->ObserverBase + SPMI_APPS_EE * SPMI_OBSERVER_EE_STRIDE +
                               (ULONG64)Apid * SPMI_OBSERVER_STRIDE);
    if (Channel == NULL || !KdDwcSpmiRun(Channel, SPMI_COMMAND(SPMI_OPCODE_READ, Address)))
        return FALSE;

    *Value = (UCHAR)KdDwcRead(Channel, SPMI_CHANNEL_READ_DATA);
    return TRUE;
}

/**
 * @brief Enables a PMIC eUSB2 repeater, tunes it and leaves it set for device mode.
 *
 * Firmware runs the port as a host and forces the repeater's 19.2 MHz clock
 * on for it, which a device must not keep. The repeater has no reset, so the
 * force is cleared by hand.
 *
 * @param[in] Base
 * The repeater peripheral's address on its PMIC.
 *
 * @param[in] Tuning
 * Board tuning writes, offsets inside the peripheral.
 */
BOOLEAN
NTAPI
KdDwcStartEusb2Repeater(
    _In_ const KDDWC_SPMI* Spmi,
    _In_ ULONG Bus,
    _In_ UCHAR Sid,
    _In_ USHORT Base,
    _In_reads_(Count) const KDDWC_REG_WRITE* Tuning,
    _In_ ULONG Count)
{
    UCHAR Status = 0;
    ULONG Waited;
    ULONG i;

    if (!KdDwcSpmiWrite(Spmi, Bus, Sid, Base + EUSB2_ENABLE, EUSB2_ENABLE_REPEATER))
        return FALSE;

    for (i = 0; i < Count; i++)
    {
        if (!KdDwcSpmiWrite(Spmi, Bus, Sid, Base + Tuning[i].Offset, Tuning[i].Value))
            return FALSE;
    }

    for (Waited = 0; Waited < EUSB2_READY_TIMEOUT_US; Waited += 10)
    {
        if (KdDwcSpmiRead(Spmi, Bus, Sid, Base + EUSB2_STATUS, &Status) && (Status & EUSB2_STATUS_READY))
            break;

        KdDwcStall(10);
    }

    if (!(Status & EUSB2_STATUS_READY))
    {
        KdDwcTrace("eUSB2 repeater 0x%x on SID %u not ready: 0x%x\n", Base, Sid, Status);
        return FALSE;
    }

    return KdDwcSpmiWrite(Spmi, Bus, Sid, Base + EUSB2_FORCE_ENABLE_5, 0) &&
           KdDwcSpmiWrite(Spmi, Bus, Sid, Base + EUSB2_FORCE_VALUE_5, 0);
}
