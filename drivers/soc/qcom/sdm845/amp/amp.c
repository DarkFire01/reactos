/*
 * PROJECT:     ReactOS Qualcomm audio support (SDM845)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     MAX98927 speaker amplifier on the quaternary MI2S link
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The OnePlus 6 loudspeaker is driven by a MAX98927 on QUPv3 wrap0 SE4 (I2C,
 * address 0x3A) that takes I2S from the LPASS quaternary MI2S port. The ADSP
 * clocks that port, so the amp is a plain I2S slave: configure it once while
 * the link is idle, then switch the amp and the global enable on after the
 * AFE port runs. The 6T uses a TFA9894 instead, which is not handled here.
 */

#include <ntddk.h>
#include <qcom/geniif.h>
#include <qcom/tlmmif.h>
#include <qcom/ampif.h>

#define QCDBG_COMPONENT "qcomamp"
#include <qcom/qcomdbg.h>

#define AMP_I2C_ADDRESS             0x3A
#define AMP_TLMM_BASE               0x03400000
#define AMP_TLMM_LENGTH             0xC00000
#define AMP_TILE_EAST               0x100000
#define AMP_TILE_NORTH              0x500000
#define AMP_TILE_SOUTH              0x900000

/* Register map (MAX98927 data sheet) */
#define AMP_REG_PCM_RX_EN_A         0x0018
#define AMP_REG_PCM_TX_EN_A         0x001A
#define AMP_REG_PCM_TX_HIZ_A        0x001C
#define AMP_REG_PCM_TX_HIZ_B        0x001D
#define AMP_REG_PCM_TX_SRC_A        0x001E
#define AMP_REG_PCM_MODE            0x0020
#define AMP_REG_PCM_MASTER          0x0021
#define AMP_REG_PCM_CLOCK           0x0022
#define AMP_REG_PCM_RATE1           0x0023
#define AMP_REG_PCM_RATE2           0x0024
#define AMP_REG_MONOMIX_A           0x0025
#define AMP_REG_MONOMIX_B           0x0026
#define AMP_REG_PDM_RX              0x0035
#define AMP_REG_VOLUME              0x0036
#define AMP_REG_DSP_CFG             0x0037
#define AMP_REG_AMP_ENABLE          0x003A
#define AMP_REG_SPK_SOURCE          0x003B
#define AMP_REG_SPK_GAIN            0x003C
#define AMP_REG_MEAS_DSP            0x003F
#define AMP_REG_BOOST0              0x0040
#define AMP_REG_BOOST1              0x0042
#define AMP_REG_MEAS_ADC            0x0043
#define AMP_REG_MEAS_BASE_HI        0x0044
#define AMP_REG_MEAS_BASE_LO        0x0045
#define AMP_REG_BROWNOUT4           0x007F
#define AMP_REG_ENV_HEADROOM        0x0082
#define AMP_REG_ENV_CTRL            0x0086
#define AMP_REG_ENV_BOOST_READ      0x0087
#define AMP_REG_GLOBAL_ENABLE       0x00FF
#define AMP_REG_SOFT_RESET          0x0100
#define AMP_REG_REVISION            0x01FF

/* The voltage and current sense slots of this board */
#define AMP_VMON_SLOT               1
#define AMP_IMON_SLOT               0

/* Digital volume: 0.25 dB per step from -16 dB at 0x00; the default 0x38 is the loudest we allow */
#define AMP_VOLUME_DEFAULT          0x38
#define AMP_VOLUME_STEP_DB          (65536 / 4)

typedef struct _AMP_PIN
{
    ULONG Number;
    ULONG Tile;
    ULONG Function;
    QCOM_TLMM_PULL Pull;
    ULONG Drive;
    QCOM_TLMM_DIRECTION Direction;
} AMP_PIN;

static const AMP_PIN AmpPins[] =
{
    /* Quaternary MI2S: SCK, WS, SD0 (sense back), SD1 (playback) */
    { 58, AMP_TILE_NORTH, 1, TlmmPullNone, 8, TlmmInput },
    { 59, AMP_TILE_NORTH, 1, TlmmPullNone, 8, TlmmInput },
    { 60, AMP_TILE_NORTH, 1, TlmmPullNone, 8, TlmmInput },
    { 61, AMP_TILE_NORTH, 1, TlmmPullNone, 8, TlmmInput },
    /* QUP wrap0 SE4 SDA and SCL */
    { 89, AMP_TILE_SOUTH, 2, TlmmPullUp, 2, TlmmInput },
    { 90, AMP_TILE_SOUTH, 3, TlmmPullUp, 2, TlmmInput },
    /* Amp reset, active low, released high */
    { 69, AMP_TILE_EAST, 0, TlmmPullUp, 16, TlmmOutputHigh },
};

typedef struct _AMP_WRITE
{
    USHORT Reg;
    UCHAR Value;
} AMP_WRITE;

/* Static setup after the soft reset: sense slots parked, mono mix, +13 dB, boost and tracking limits */
static const AMP_WRITE AmpSetup[] =
{
    { AMP_REG_PCM_TX_HIZ_A,     0xFF },
    { AMP_REG_PCM_TX_HIZ_B,     0xFF },
    { AMP_REG_MONOMIX_A,        0x80 },
    { AMP_REG_MONOMIX_B,        0x01 },
    { AMP_REG_VOLUME,           AMP_VOLUME_DEFAULT },
    { AMP_REG_SPK_GAIN,         0x05 },
    { AMP_REG_DSP_CFG,          0x03 },
    { AMP_REG_MEAS_DSP,         0xF7 },
    { AMP_REG_BOOST0,           0x1C },
    { AMP_REG_BOOST1,           0x3E },
    { AMP_REG_MEAS_ADC,         0x04 },
    { AMP_REG_MEAS_BASE_HI,     0x00 },
    { AMP_REG_MEAS_BASE_LO,     0x24 },
    { AMP_REG_BROWNOUT4,        0x06 },
    { AMP_REG_ENV_HEADROOM,     0x08 },
    { AMP_REG_ENV_CTRL,         0x01 },
    { AMP_REG_ENV_BOOST_READ,   0x10 },
    { AMP_REG_PCM_TX_SRC_A,     (AMP_IMON_SLOT << 4) | AMP_VMON_SLOT },
};

typedef struct _AMP_STATE
{
    FAST_MUTEX Lock;
    BOOLEAN Ready;
    BOOLEAN Running;
    PQCOM_TLMM Tlmm;
    PQCOM_GENI_I2C I2c;
    UCHAR Volume;
    BOOLEAN Muted;
} AMP_STATE;

static AMP_STATE AmpState;

static
NTSTATUS
AmpWrite(
    _In_ USHORT Reg,
    _In_ UCHAR Value)
{
    UCHAR Buffer[3];

    Buffer[0] = (UCHAR)(Reg >> 8);
    Buffer[1] = (UCHAR)Reg;
    Buffer[2] = Value;
    return QcGeniI2cTransfer(AmpState.I2c, AMP_I2C_ADDRESS, Buffer, sizeof(Buffer), NULL, 0);
}

static
NTSTATUS
AmpRead(
    _In_ USHORT Reg,
    _Out_ PUCHAR Value)
{
    UCHAR Address[2];

    Address[0] = (UCHAR)(Reg >> 8);
    Address[1] = (UCHAR)Reg;
    return QcGeniI2cTransfer(AmpState.I2c, AMP_I2C_ADDRESS, Address, sizeof(Address), Value, 1);
}

/* Read, clear Mask, or in Value, write back */
static
NTSTATUS
AmpUpdate(
    _In_ USHORT Reg,
    _In_ UCHAR Mask,
    _In_ UCHAR Value)
{
    UCHAR Current;
    NTSTATUS Status;

    Status = AmpRead(Reg, &Current);
    if (!NT_SUCCESS(Status))
        return Status;

    return AmpWrite(Reg, (UCHAR)((Current & ~Mask) | (Value & Mask)));
}

static
VOID
AmpConfigurePins(VOID)
{
    QCOM_TLMM_PIN Pin;
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(AmpPins); i++)
    {
        RtlZeroMemory(&Pin, sizeof(Pin));
        Pin.Number = AmpPins[i].Number;
        Pin.TileOffset = AmpPins[i].Tile;
        Pin.Function = AmpPins[i].Function;
        Pin.Pull = AmpPins[i].Pull;
        Pin.DriveMilliamps = AmpPins[i].Drive;
        Pin.Direction = AmpPins[i].Direction;
        QcTlmmConfigurePin(AmpState.Tlmm, &Pin);
    }
}

static
NTSTATUS
AmpBringUp(VOID)
{
    QCOM_GENI_I2C_CONFIG Config;
    PHYSICAL_ADDRESS Base;
    LARGE_INTEGER Delay;
    UCHAR Revision = 0;
    NTSTATUS Status;
    ULONG i;

    Base.QuadPart = AMP_TLMM_BASE;
    Status = QcTlmmConnect(Base, AMP_TLMM_LENGTH, &AmpState.Tlmm);
    if (!NT_SUCCESS(Status))
        return Status;

    AmpConfigurePins();

    /* The amp needs a few ms after reset before its I2C port answers */
    Delay.QuadPart = -10LL * 1000 * 6;
    KeDelayExecutionThread(KernelMode, FALSE, &Delay);

    /* Wrap0 SE4 at 100 kHz: SE clock vote bit 14, wrap0 master and slave AHB bits 6 and 7 */
    RtlZeroMemory(&Config, sizeof(Config));
    Config.EngineBase.QuadPart = 0x00890000;
    Config.ClockControllerBase.QuadPart = 0x00100000;
    Config.ClockVoteOffset = 0x5200C;
    Config.ClockVoteMask = 0x000040C0;
    Config.EngineBranchOffset = 0x174F0;
    Config.EngineRcgOffset = 0x174F4;
    Config.BusSpeedHz = 100000;

    Status = QcGeniI2cOpen(&Config, &AmpState.I2c);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = AmpWrite(AMP_REG_SOFT_RESET, 0x01);
    if (!NT_SUCCESS(Status))
    {
        QC_LOG("max98927 does not answer at 0x%02x: 0x%08lx", AMP_I2C_ADDRESS, Status);
        return Status;
    }

    Delay.QuadPart = -10LL * 1000 * 2;
    KeDelayExecutionThread(KernelMode, FALSE, &Delay);

    Status = AmpRead(AMP_REG_REVISION, &Revision);
    if (!NT_SUCCESS(Status))
        return Status;

    for (i = 0; i < RTL_NUMBER_OF(AmpSetup); i++)
    {
        Status = AmpWrite(AmpSetup[i].Reg, AmpSetup[i].Value);
        if (!NT_SUCCESS(Status))
        {
            QC_LOG("max98927 write 0x%04x failed 0x%08lx", AmpSetup[i].Reg, Status);
            return Status;
        }
    }

    /* Sense slots out of high impedance and onto the tx line */
    AmpUpdate(AMP_REG_PCM_TX_HIZ_A, (1 << AMP_VMON_SLOT) | (1 << AMP_IMON_SLOT), 0);
    AmpUpdate(AMP_REG_PCM_TX_EN_A, (1 << AMP_VMON_SLOT) | (1 << AMP_IMON_SLOT),
              (1 << AMP_VMON_SLOT) | (1 << AMP_IMON_SLOT));

    /* I2S slave, normal clock polarity, both rx channels into the mono mix, pcm source */
    AmpUpdate(AMP_REG_PCM_MASTER, 0x03, 0x00);
    AmpUpdate(AMP_REG_PCM_MODE, 0x3C, 0x00);
    AmpUpdate(AMP_REG_PCM_RX_EN_A, 0x03, 0x03);
    AmpUpdate(AMP_REG_SPK_SOURCE, 0x03, 0x00);
    AmpUpdate(AMP_REG_PDM_RX, 0x01, 0x00);

    AmpWrite(AMP_REG_VOLUME, AmpState.Volume);
    QC_LOG("max98927 revision 0x%02x configured", Revision);
    return STATUS_SUCCESS;
}

/* Sample rate code of the PCM rate registers */
static
UCHAR
AmpRateCode(
    _In_ ULONG Rate)
{
    static const ULONG Rates[] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 };
    UCHAR i;

    for (i = 0; i < RTL_NUMBER_OF(Rates); i++)
    {
        if (Rates[i] == Rate)
            return i;
    }
    return 8;
}

/* Bit clocks per frame code: 32 is 2, then 48, 64, 96, 128, 192, 256, 384, 512 */
static
UCHAR
AmpBclkCode(
    _In_ ULONG BitsPerFrame)
{
    static const ULONG Ratios[] = { 32, 48, 64, 96, 128, 192, 256, 384, 512 };
    UCHAR i;

    for (i = 0; i < RTL_NUMBER_OF(Ratios); i++)
    {
        if (Ratios[i] == BitsPerFrame)
            return i + 2;
    }
    return 2;
}

NTSTATUS
NTAPI
QcAmpPrepare(VOID)
{
    NTSTATUS Status = STATUS_SUCCESS;

    PAGED_CODE();

    ExAcquireFastMutex(&AmpState.Lock);
    if (!AmpState.Ready)
    {
        Status = AmpBringUp();
        if (NT_SUCCESS(Status))
            AmpState.Ready = TRUE;
    }
    ExReleaseFastMutex(&AmpState.Lock);
    return Status;
}

NTSTATUS
NTAPI
QcAmpStart(
    _In_ ULONG Rate,
    _In_ ULONG Bits,
    _In_ ULONG Channels)
{
    UCHAR ChannelSize, RateCode;
    NTSTATUS Status;

    PAGED_CODE();

    ExAcquireFastMutex(&AmpState.Lock);
    if (!AmpState.Ready)
    {
        ExReleaseFastMutex(&AmpState.Lock);
        return STATUS_DEVICE_NOT_READY;
    }

    ChannelSize = (Bits == 32) ? 0xC0 : (Bits == 24) ? 0x80 : 0x40;
    RateCode = AmpRateCode(Rate);

    AmpUpdate(AMP_REG_PCM_MODE, 0xC0, ChannelSize);
    AmpUpdate(AMP_REG_PCM_RATE1, 0x0F, RateCode);
    AmpUpdate(AMP_REG_PCM_RATE2, 0xFF, (UCHAR)((RateCode << 4) | RateCode));
    AmpUpdate(AMP_REG_PCM_CLOCK, 0x0F, AmpBclkCode(Bits * Channels));

    AmpUpdate(AMP_REG_AMP_ENABLE, 0x01, AmpState.Muted ? 0x00 : 0x01);
    Status = AmpUpdate(AMP_REG_GLOBAL_ENABLE, 0x01, 0x01);
    if (NT_SUCCESS(Status))
    {
        AmpState.Running = TRUE;
        QC_LOG("max98927 on: %lu Hz, %lu bit, %lu slots", Rate, Bits, Channels);
    }

    ExReleaseFastMutex(&AmpState.Lock);
    return Status;
}

VOID
NTAPI
QcAmpStop(VOID)
{
    PAGED_CODE();

    ExAcquireFastMutex(&AmpState.Lock);
    if (AmpState.Running)
    {
        AmpUpdate(AMP_REG_GLOBAL_ENABLE, 0x01, 0x00);
        AmpUpdate(AMP_REG_AMP_ENABLE, 0x01, 0x00);
        AmpState.Running = FALSE;
        QC_LOG("max98927 off");
    }
    ExReleaseFastMutex(&AmpState.Lock);
}

/* Level is in 1/65536 dB below the default, the amp steps by 0.25 dB down to 14 dB under it */
VOID
NTAPI
QcAmpSetVolume(
    _In_ LONG Level)
{
    LONG Steps;

    PAGED_CODE();

    if (Level > 0)
        Level = 0;

    Steps = -Level / AMP_VOLUME_STEP_DB;
    if (Steps > AMP_VOLUME_DEFAULT)
        Steps = AMP_VOLUME_DEFAULT;

    ExAcquireFastMutex(&AmpState.Lock);
    AmpState.Volume = (UCHAR)(AMP_VOLUME_DEFAULT - Steps);
    if (AmpState.Ready)
        AmpWrite(AMP_REG_VOLUME, AmpState.Volume);
    ExReleaseFastMutex(&AmpState.Lock);
}

/* The digital volume has no off step, so mute gates the amp output stage */
VOID
NTAPI
QcAmpSetMute(
    _In_ BOOLEAN Mute)
{
    PAGED_CODE();

    ExAcquireFastMutex(&AmpState.Lock);
    AmpState.Muted = Mute;
    if (AmpState.Running)
        AmpUpdate(AMP_REG_AMP_ENABLE, 0x01, Mute ? 0x00 : 0x01);
    ExReleaseFastMutex(&AmpState.Lock);
}

NTSTATUS
NTAPI
DllInitialize(
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    /* Runs when the first importer loads us, before any export is called */
    ExInitializeFastMutex(&AmpState.Lock);
    AmpState.Volume = AMP_VOLUME_DEFAULT;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DllUnload(VOID)
{
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);
    return STATUS_SUCCESS;
}
