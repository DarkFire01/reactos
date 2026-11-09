/*
 * PROJECT:     ReactOS Qualcomm subsystem manager
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Modem clocks, resets, Q6 power up and image loader requests
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "subsys.h"
#include <ntstrsafe.h>

#define NDEBUG
#include <debug.h>

/* CBCR layout, bit0 enables the branch, bit31 reports the branch stopped */
#define SS_CBCR_ENABLE              0x00000001
#define SS_CBCR_CLK_OFF             0x80000000

/* MSS Q6 clock vote, bit 27 */
#define SS_MSS_CLOCK_VOTE           0x08000000

/* Halt handshake request level */
#define SS_HALT_REQUEST             1

/* Relative wait expressed in 100 ns units */
#define SS_MS(ms)                   (-(LONGLONG)(ms) * 10 * 1000)
#define SS_RMB_POLL_MS              100
#define SS_HALT_DELAY_MS            100

/* Clock off poll, 5000 reads two microseconds apart */
#define SS_CLK_OFF_RETRIES          5000
#define SS_CLK_OFF_STALL_US         2

/* Physical bases of every mapped register, indexed by SS_REG */
static const ULONG SsRegPhys[SsRegCount] =
{
    0xFC400280, /* SsRegMssCfgAhbCbcr */
    0xFC400284, /* SsRegMssQ6AxiCbcr */
    0xFC401484, /* SsRegMssClockVote */
    0xFC401680, /* SsRegMssRestart */
    0xFC880014, /* SsRegQ6Reset */
    0xFC880020, /* SsRegQ6GfmuxCtl */
    0xFC880030, /* SsRegQ6PwrCtl */
    0xFC880038, /* SsRegQ6XoCbcr */
    0xFC880110, /* SsRegQ6StrapAcc */
    0xFD485180, /* SsRegHaltQ6Req */
    0xFD485184, /* SsRegHaltQ6Ack */
    0xFD485188, /* SsRegHaltQ6Idle */
    0xFD485200, /* SsRegHaltModemReq */
    0xFD485204, /* SsRegHaltModemAck */
    0xFD485208, /* SsRegHaltModemIdle */
    0xFD485280, /* SsRegHaltNcReq */
    0xFD485284, /* SsRegHaltNcAck */
    0xFD485288  /* SsRegHaltNcIdle */
};

#define SS_RMB_PHYS                 0xFC820000

/* The register accessors carry a leading dsb, the modem side also wants the
 * completion fenced so the next poke cannot pass this one */
static
ULONG
SsRead(
    _In_ PSS_DEVICE Device,
    _In_ SS_REG Reg)
{
    ULONG Value = READ_REGISTER_ULONG(Device->Regs[Reg]);
    __dmb(0xB);
    return Value;
}

static
VOID
SsWrite(
    _In_ PSS_DEVICE Device,
    _In_ SS_REG Reg,
    _In_ ULONG Value)
{
    WRITE_REGISTER_ULONG(Device->Regs[Reg], Value);
    __dmb(0xB);
}

static
ULONG
SsReadRmb(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Offset)
{
    ULONG Value = READ_REGISTER_ULONG((PULONG)(Device->Rmb + Offset));
    __dmb(0xB);
    return Value;
}

NTSTATUS
SsMapRegisters(
    _Inout_ PSS_DEVICE Device)
{
    PHYSICAL_ADDRESS Address;
    ULONG i;

    for (i = 0; i < SsRegCount; i++)
    {
        Address.QuadPart = SsRegPhys[i];
        Device->Regs[i] = MmMapIoSpace(Address, sizeof(ULONG), MmNonCached);
        if (!Device->Regs[i])
        {
            DPRINT1("[subsys] map of reg %lu (0x%08lx) FAILED\n", i, SsRegPhys[i]);
            SsUnmapRegisters(Device);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    Address.QuadPart = SS_RMB_PHYS;
    Device->Rmb = MmMapIoSpace(Address, QCOM_RMB_SIZE, MmNonCached);
    if (!Device->Rmb)
    {
        DPRINT1("[subsys] map of RMB 0x%08lx FAILED\n", SS_RMB_PHYS);
        SsUnmapRegisters(Device);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    DPRINT1("[subsys] mapped %lu MSS regs, RMB at %p\n", (ULONG)SsRegCount, Device->Rmb);
    return STATUS_SUCCESS;
}

VOID
SsUnmapRegisters(
    _Inout_ PSS_DEVICE Device)
{
    ULONG i;

    for (i = 0; i < SsRegCount; i++)
    {
        if (Device->Regs[i])
        {
            MmUnmapIoSpace(Device->Regs[i], sizeof(ULONG));
            Device->Regs[i] = NULL;
        }
    }

    if (Device->Rmb)
    {
        MmUnmapIoSpace(Device->Rmb, QCOM_RMB_SIZE);
        Device->Rmb = NULL;
    }
}

VOID
SsArmWatchdog(
    _Inout_ PSS_DEVICE Device)
{
    WdfInterruptEnable(Device->Watchdog);
    InterlockedExchange(&Device->WatchdogArmed, 1);
    DPRINT1("[subsys] watchdog armed\n");
}

VOID
SsMaskWatchdog(
    _Inout_ PSS_DEVICE Device)
{
    InterlockedExchange(&Device->WatchdogArmed, 0);
    WdfInterruptDisable(Device->Watchdog);
    DPRINT1("[subsys] watchdog masked\n");
}

/* Fills one MSA record and hands it to the image loader, no output expected */
static
NTSTATUS
SsSendMsa(
    _In_ PSS_DEVICE Device,
    _In_ PVOID Buffer,
    _In_ ULONG Length)
{
    WDF_MEMORY_DESCRIPTOR InDesc;
    NTSTATUS Status;

    if (Device->Pil == NULL)
    {
        DPRINT1("[subsys] PIL MSA: no PIL target\n");
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&InDesc, Buffer, Length);

    DPRINT1("[subsys] PIL MSA type %lu (%lu bytes) ->\n", ((PQCOM_PIL_MSA_HEADER)Buffer)->Type, Length);
    Status = WdfIoTargetSendIoctlSynchronously(Device->Pil,
                                               NULL,
                                               IOCTL_QCOM_PIL_MSA,
                                               &InDesc,
                                               NULL,
                                               NULL,
                                               NULL);
    DPRINT1("[subsys] PIL MSA type %lu <- 0x%lx\n", ((PQCOM_PIL_MSA_HEADER)Buffer)->Type, Status);
    return Status;
}

static
NTSTATUS
SsSendMsaRmb(
    _Inout_ PSS_DEVICE Device)
{
    RtlZeroMemory(&Device->MsaRmb, sizeof(Device->MsaRmb));
    Device->MsaRmb.Header.Size = (USHORT)sizeof(Device->MsaRmb);
    Device->MsaRmb.Header.Version = QCOM_PIL_MSA_VERSION;
    Device->MsaRmb.Header.Type = QCOM_PIL_MSA_SET_RMB;
    Device->MsaRmb.Registers = Device->Rmb;

    return SsSendMsa(Device, &Device->MsaRmb, sizeof(Device->MsaRmb));
}

static
NTSTATUS
SsSendMsaMba(
    _Inout_ PSS_DEVICE Device)
{
    RtlZeroMemory(&Device->MsaMba, sizeof(Device->MsaMba));
    Device->MsaMba.Header.Size = (USHORT)sizeof(Device->MsaMba);
    Device->MsaMba.Header.Version = QCOM_PIL_MSA_VERSION;
    Device->MsaMba.Header.Type = QCOM_PIL_MSA_LOAD_MBA;
    Device->MsaMba.Image = GUID_QCOM_IMAGE_MBA;
    RtlStringCchCopyW(Device->MsaMba.Path, QCOM_PIL_PATH_LENGTH, Device->FwImagePath);

    return SsSendMsa(Device, &Device->MsaMba, sizeof(Device->MsaMba));
}

static
NTSTATUS
SsSendMsaModem(
    _Inout_ PSS_DEVICE Device)
{
    RtlZeroMemory(&Device->MsaModem, sizeof(Device->MsaModem));
    Device->MsaModem.Header.Size = (USHORT)sizeof(Device->MsaModem);
    Device->MsaModem.Header.Version = QCOM_PIL_MSA_VERSION;
    Device->MsaModem.Header.Type = QCOM_PIL_MSA_LOAD_MODEM;
    Device->MsaModem.Image = GUID_QCOM_IMAGE_MODEM;
    RtlStringCchCopyW(Device->MsaModem.Path, QCOM_PIL_PATH_LENGTH, Device->SwImagePath);

    return SsSendMsa(Device, &Device->MsaModem, sizeof(Device->MsaModem));
}

static
NTSTATUS
SsSendUnload(
    _In_ PSS_DEVICE Device,
    _In_ const GUID *Image)
{
    WDF_MEMORY_DESCRIPTOR InDesc;
    GUID Local = *Image;
    NTSTATUS Status;

    if (Device->Pil == NULL)
        return STATUS_DEVICE_NOT_CONNECTED;

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&InDesc, &Local, sizeof(Local));

    Status = WdfIoTargetSendIoctlSynchronously(Device->Pil,
                                               NULL,
                                               IOCTL_QCOM_PIL_UNLOAD,
                                               &InDesc,
                                               NULL,
                                               NULL,
                                               NULL);
    DPRINT1("[subsys] PIL unload {%08lx-...} -> 0x%lx\n", Local.Data1, Status);
    return Status;
}

/* 8992 and 8994 share this sequence, each line is a single write of a value
 * carried over from the line before, no read back in between */
static
VOID
SsQ6PowerUp(
    _In_ PSS_DEVICE Device)
{
    ULONG Value;

    switch (Device->ChipFamily)
    {
        case SS_FAMILY_8992:
        case SS_FAMILY_8994:
            SsWrite(Device, SsRegQ6StrapAcc, 0x20);
            SsWrite(Device, SsRegQ6XoCbcr, 1);

            Value = 0x01700000;
            SsWrite(Device, SsRegQ6PwrCtl, Value);
            Value |= 0x02000000;
            SsWrite(Device, SsRegQ6PwrCtl, Value);
            Value &= ~0x00400000;
            SsWrite(Device, SsRegQ6PwrCtl, Value);
            Value |= 0x000FFFFF;
            SsWrite(Device, SsRegQ6PwrCtl, Value);
            Value &= ~0x00200000;
            SsWrite(Device, SsRegQ6PwrCtl, Value);
            Value &= ~0x00100000;
            SsWrite(Device, SsRegQ6PwrCtl, Value);

            SsWrite(Device, SsRegQ6Reset, 4);
            SsWrite(Device, SsRegQ6GfmuxCtl, 0x102);
            break;

        default:
            DPRINT1("[subsys] No Q6 power up for chip family 0x%lx\n", Device->ChipFamily);
            return;
    }

    DPRINT1("[subsys] Q6 powered: PWR_CTL=0x%08lx RESET=0x%08lx GFMUX=0x%08lx\n",
            SsRead(Device, SsRegQ6PwrCtl), SsRead(Device, SsRegQ6Reset), SsRead(Device, SsRegQ6GfmuxCtl));
}

/* PBL hands over once its status leaves the value seen at reset */
static
NTSTATUS
SsWaitPbl(
    _In_ PSS_DEVICE Device,
    _In_ ULONG First)
{
    LARGE_INTEGER Delay;
    ULONG Current;
    ULONG Waited;

    Delay.QuadPart = SS_MS(SS_RMB_POLL_MS);

    for (Waited = 0; Waited < Device->RmbTimeoutMs; Waited += SS_RMB_POLL_MS)
    {
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);

        Current = SsReadRmb(Device, QCOM_RMB_PBL_STATUS);
        if (Current != First)
        {
            DPRINT1("[subsys] PBL status 0x%08lx -> 0x%08lx after %lu ms\n", First, Current, Waited + SS_RMB_POLL_MS);
            if ((LONG)Current < 0)
                return STATUS_UNSUCCESSFUL;
            return STATUS_SUCCESS;
        }
    }

    DPRINT1("[subsys] PBL status TIMEOUT, stuck at 0x%08lx\n", First);
    return STATUS_UNSUCCESSFUL;
}

/* MBA reports healthy with a positive status */
static
NTSTATUS
SsWaitMba(
    _In_ PSS_DEVICE Device)
{
    LARGE_INTEGER Delay;
    ULONG Current = 0;
    ULONG Waited;

    Delay.QuadPart = SS_MS(SS_RMB_POLL_MS);

    for (Waited = 0; Waited < Device->RmbTimeoutMs; Waited += SS_RMB_POLL_MS)
    {
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);

        Current = SsReadRmb(Device, QCOM_RMB_MBA_STATUS);
        if ((LONG)Current > 0)
        {
            DPRINT1("[subsys] MBA status 0x%08lx after %lu ms\n", Current, Waited + SS_RMB_POLL_MS);
            return STATUS_SUCCESS;
        }
    }

    DPRINT1("[subsys] MBA status TIMEOUT, last 0x%08lx\n", Current);
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS
SsBringUpMba(
    _Inout_ PSS_DEVICE Device)
{
    NTSTATUS Status;
    BOOLEAN Voted;
    BOOLEAN ClockDone;
    ULONG Pbl;
    ULONG Mba;

    if (Device->Pil == NULL)
    {
        DPRINT1("[subsys] MBA bring up: no PIL target\n");
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    DPRINT1("[subsys] MBA bring up begin\n");
    SsArmWatchdog(Device);

    Voted = (Device->ChipFamily == SS_FAMILY_8992 || Device->ChipFamily == SS_FAMILY_8994);

    /* Release the MSS reset and raise the Q6 AXI clock */
    SsWrite(Device, SsRegMssRestart, 0);
    KeStallExecutionProcessor(2);
    SsWrite(Device, SsRegMssQ6AxiCbcr, SsRead(Device, SsRegMssQ6AxiCbcr) | SS_CBCR_ENABLE);
    DPRINT1("[subsys]   MSS reset released, Q6 AXI CBCR=0x%08lx\n", SsRead(Device, SsRegMssQ6AxiCbcr));

    ClockDone = FALSE;
    if (Voted)
    {
        SsWrite(Device, SsRegMssClockVote, SsRead(Device, SsRegMssClockVote) | SS_MSS_CLOCK_VOTE);
        ClockDone = SsPowerVote(Device, SS_VOTE_MSS_CLOCK);
        DPRINT1("[subsys]   MSS clock vote=0x%08lx, power engine took AHB: %u\n",
                SsRead(Device, SsRegMssClockVote), ClockDone);
    }

    /* The power engine owns the config AHB clock once the vote lands */
    if (!ClockDone)
    {
        SsWrite(Device, SsRegMssCfgAhbCbcr, SsRead(Device, SsRegMssCfgAhbCbcr) | SS_CBCR_ENABLE);
        DPRINT1("[subsys]   MSS cfg AHB CBCR=0x%08lx\n", SsRead(Device, SsRegMssCfgAhbCbcr));
    }

    Status = SsSendMsaRmb(Device);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = SsSendMsaMba(Device);
    if (!NT_SUCCESS(Status))
        return Status;

    SsQ6PowerUp(Device);

    Pbl = SsReadRmb(Device, QCOM_RMB_PBL_STATUS);
    Mba = SsReadRmb(Device, QCOM_RMB_MBA_STATUS);
    DPRINT1("[subsys]   RMB: MBA_IMAGE=0x%08lx PBL=0x%08lx MBA=0x%08lx\n",
            SsReadRmb(Device, QCOM_RMB_MBA_IMAGE), Pbl, Mba);

    if (Pbl != 1)
    {
        Status = SsWaitPbl(Device, Pbl);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    if ((LONG)Mba <= 0)
    {
        Status = SsWaitMba(Device);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
SsLoadModemImage(
    _Inout_ PSS_DEVICE Device)
{
    NTSTATUS Status;

    if (Device->MsaMba.Header.Type != QCOM_PIL_MSA_LOAD_MBA)
    {
        DPRINT1("[subsys] modem image: MBA record not loaded (type %lu)\n", Device->MsaMba.Header.Type);
        return STATUS_INVALID_DEVICE_STATE;
    }

    Status = SsSendMsaModem(Device);
    DPRINT1("[subsys]   RMB after modem load: CMD=0x%08lx MBA=0x%08lx META=0x%08lx\n",
            SsReadRmb(Device, QCOM_RMB_MBA_COMMAND),
            SsReadRmb(Device, QCOM_RMB_MBA_STATUS),
            SsReadRmb(Device, QCOM_RMB_PMI_META_DATA));
    return Status;
}

NTSTATUS
SsUnloadImages(
    _Inout_ PSS_DEVICE Device)
{
    NTSTATUS Mba;
    NTSTATUS Modem;

    Mba = SsSendUnload(Device, &GUID_QCOM_IMAGE_MBA);
    Modem = SsSendUnload(Device, &GUID_QCOM_IMAGE_MODEM);

    return NT_SUCCESS(Mba) ? Modem : Mba;
}

static
VOID
SsHaltPort(
    _In_ PSS_DEVICE Device,
    _In_ SS_REG Req,
    _In_ SS_REG Ack,
    _In_ SS_REG Idle)
{
    LARGE_INTEGER Delay;

    SsWrite(Device, Req, SS_HALT_REQUEST);

    if (KeGetCurrentIrql() == PASSIVE_LEVEL)
    {
        Delay.QuadPart = SS_MS(SS_HALT_DELAY_MS);
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }

    DPRINT1("[subsys] AXI halt port %lu: ack 0x%lx idle 0x%lx\n", (ULONG)Req, SsRead(Device, Ack), SsRead(Device, Idle));

    SsWrite(Device, Req, 0);
}

VOID
SsHaltAxi(
    _Inout_ PSS_DEVICE Device)
{
    SsHaltPort(Device, SsRegHaltQ6Req, SsRegHaltQ6Ack, SsRegHaltQ6Idle);
    SsHaltPort(Device, SsRegHaltModemReq, SsRegHaltModemAck, SsRegHaltModemIdle);
    SsHaltPort(Device, SsRegHaltNcReq, SsRegHaltNcAck, SsRegHaltNcIdle);
}

VOID
SsAssertReset(
    _Inout_ PSS_DEVICE Device)
{
    SsWrite(Device, SsRegMssRestart, 1);
    KeStallExecutionProcessor(1);
    DPRINT1("[subsys] MSS reset asserted\n");
}

static
VOID
SsWaitClockOff(
    _In_ PSS_DEVICE Device,
    _In_ SS_REG Reg)
{
    ULONG i;

    for (i = 0; i < SS_CLK_OFF_RETRIES; i++)
    {
        if (SsRead(Device, Reg) & SS_CBCR_CLK_OFF)
            break;
        KeStallExecutionProcessor(SS_CLK_OFF_STALL_US);
    }
}

VOID
SsClocksOff(
    _Inout_ PSS_DEVICE Device)
{
    if (Device->ChipFamily == SS_FAMILY_8992 || Device->ChipFamily == SS_FAMILY_8994)
    {
        SsWrite(Device, SsRegMssClockVote, SsRead(Device, SsRegMssClockVote) & ~SS_MSS_CLOCK_VOTE);
        DPRINT1("[subsys] MSS clock vote dropped, vote=0x%08lx\n", SsRead(Device, SsRegMssClockVote));
        return;
    }

    SsWrite(Device, SsRegMssCfgAhbCbcr, SsRead(Device, SsRegMssCfgAhbCbcr) & ~SS_CBCR_ENABLE);
    SsWaitClockOff(Device, SsRegMssCfgAhbCbcr);

    SsWrite(Device, SsRegMssQ6AxiCbcr, SsRead(Device, SsRegMssQ6AxiCbcr) & ~SS_CBCR_ENABLE);
    SsWaitClockOff(Device, SsRegMssQ6AxiCbcr);
}
