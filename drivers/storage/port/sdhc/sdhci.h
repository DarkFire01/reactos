/*
 * PROJECT:     ReactOS Storage Stack / SD Standard Host Controller miniport
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     SD Host Controller (SDHCI) register core shared by miniports
 * COPYRIGHT:   Justin Miller (justinmiller100@gmail.com)
 */

#ifndef _SDHCI_H_
#define _SDHCI_H_

#include <ntddk.h>
#include <sdport.h>

/* SD Host Controller Standard register offsets. */
#define SDHCI_ARGUMENT2            0x00
#define SDHCI_BLOCK_SIZE           0x04
#define SDHCI_BLOCK_COUNT          0x06
#define SDHCI_ARGUMENT             0x08
#define SDHCI_TRANSFER_MODE        0x0C
#define SDHCI_COMMAND              0x0E
#define SDHCI_RESPONSE             0x10
#define SDHCI_BUFFER_DATA_PORT     0x20
#define SDHCI_PRESENT_STATE        0x24
#define SDHCI_HOST_CONTROL1        0x28
#define SDHCI_POWER_CONTROL        0x29
#define SDHCI_BLOCK_GAP_CONTROL    0x2A
#define SDHCI_CLOCK_CONTROL        0x2C
#define SDHCI_TIMEOUT_CONTROL      0x2E
#define SDHCI_SOFTWARE_RESET       0x2F
#define SDHCI_NORMAL_INT_STATUS    0x30
#define SDHCI_ERROR_INT_STATUS     0x32
#define SDHCI_NORMAL_INT_ENABLE    0x34
#define SDHCI_ERROR_INT_ENABLE     0x36
#define SDHCI_NORMAL_SIGNAL_ENABLE 0x38
#define SDHCI_ERROR_SIGNAL_ENABLE  0x3A
#define SDHCI_HOST_CONTROL2        0x3E
#define SDHCI_CAPABILITIES         0x40
#define SDHCI_CAPABILITIES_HIGH    0x44
#define SDHCI_ADMA_ADDRESS         0x58
#define SDHCI_HOST_VERSION         0xFE

/* Present State bits. */
#define SDHCI_STATE_CMD_INHIBIT    0x00000001
#define SDHCI_STATE_DAT_INHIBIT    0x00000002
#define SDHCI_STATE_BUFFER_WRITE   0x00000400
#define SDHCI_STATE_BUFFER_READ    0x00000800
#define SDHCI_STATE_CARD_INSERTED  0x00010000
#define SDHCI_STATE_WRITE_ENABLED  0x00080000

/* Normal Interrupt Status bits (also used as the Events mask). */
#define SDHCI_INT_CMD_COMPLETE     0x0001
#define SDHCI_INT_XFER_COMPLETE    0x0002
#define SDHCI_INT_DMA              0x0008
#define SDHCI_INT_BUFFER_WRITE_RDY 0x0010
#define SDHCI_INT_BUFFER_READ_RDY  0x0020
#define SDHCI_INT_CARD_INSERT      0x0040
#define SDHCI_INT_CARD_REMOVE      0x0080
#define SDHCI_INT_CARD             0x0100
#define SDHCI_INT_RETUNE           0x1000
#define SDHCI_INT_ERROR            0x8000

/* Software Reset bits. */
#define SDHCI_RESET_ALL            0x01
#define SDHCI_RESET_CMD            0x02
#define SDHCI_RESET_DAT            0x04

/* Clock Control bits. */
#define SDHCI_CLOCK_INTERNAL_EN    0x0001
#define SDHCI_CLOCK_INTERNAL_STABLE 0x0002
#define SDHCI_CLOCK_SD_EN          0x0004

/* Power Control bits. */
#define SDHCI_POWER_ON             0x01
#define SDHCI_POWER_18             0x0A
#define SDHCI_POWER_30             0x0C
#define SDHCI_POWER_33             0x0E

/* Host Control 1 bits. */
#define SDHCI_CTRL1_DATA_4BIT      0x02
#define SDHCI_CTRL1_HIGH_SPEED     0x04
#define SDHCI_CTRL1_DATA_8BIT      0x20

/* Transfer Mode bits. */
#define SDHCI_TRANSFER_DMA_EN      0x0001
#define SDHCI_TRANSFER_BLKCNT_EN   0x0002
#define SDHCI_TRANSFER_AUTOCMD12   0x0004
#define SDHCI_TRANSFER_READ        0x0010
#define SDHCI_TRANSFER_MULTI_BLOCK 0x0020

/* Command register fields. */
#define SDHCI_CMD_DATA_PRESENT     0x0020
#define SDHCI_CMD_INDEX_CHECK      0x0010
#define SDHCI_CMD_CRC_CHECK        0x0008
#define SDHCI_CMD_RESP_NONE        0x0000
#define SDHCI_CMD_RESP_136         0x0001
#define SDHCI_CMD_RESP_48          0x0002
#define SDHCI_CMD_RESP_48_BUSY     0x0003

typedef struct _SDHCI_EXTENSION SDHCI_EXTENSION, *PSDHCI_EXTENSION;

typedef NTSTATUS (*PSDHCI_VENDOR_INITIALIZE)(
    _In_ PSDHCI_EXTENSION Extension);

typedef struct _SDHCI_EXTENSION
{
    PVOID BaseAddress;
    PSDHCI_VENDOR_INITIALIZE VendorInitialize;
    PVOID VendorContext;
    PSDPORT_REQUEST CurrentRequest;
    PUCHAR DataPointer;
    ULONG DataRemaining;
    BOOLEAN DataRead;
    BOOLEAN CrashdumpMode;
    ULONG BaseClockKhz;
    UCHAR SpecVersion;
} SDHCI_EXTENSION, *PSDHCI_EXTENSION;

/* Shared SDHCI miniport callbacks consumed by SdPortInitialize. */
NTSTATUS NTAPI SdhciGetSlotCount(_In_ PVOID Argument, _Out_ PUCHAR SlotCount);
VOID NTAPI SdhciGetSlotCapabilities(_In_ PVOID Extension, _Out_ PSDPORT_CAPABILITIES Capabilities);
NTSTATUS NTAPI SdhciInitialize(_In_ PVOID Extension, _In_ PHYSICAL_ADDRESS PhysicalBase, _In_ PVOID VirtualBase, _In_ ULONG Length, _In_ BOOLEAN CrashdumpMode);
NTSTATUS NTAPI SdhciIssueBusOperation(_In_ PVOID Extension, _In_ PSDPORT_BUS_OPERATION BusOperation);
BOOLEAN NTAPI SdhciGetCardDetectState(_In_ PVOID Extension);
BOOLEAN NTAPI SdhciGetWriteProtectState(_In_ PVOID Extension);
BOOLEAN NTAPI SdhciInterrupt(_In_ PVOID Extension, _Out_ PULONG Events, _Out_ PULONG Errors, _Out_ PBOOLEAN CardChange, _Out_ PBOOLEAN SdioInterrupt, _Out_ PBOOLEAN Tuning);
NTSTATUS NTAPI SdhciIssueRequest(_In_ PVOID Extension, _In_ PSDPORT_REQUEST Request);
VOID NTAPI SdhciGetResponse(_In_ PVOID Extension, _In_ PSDPORT_COMMAND Command, _Out_ PVOID ResponseBuffer);
VOID NTAPI SdhciToggleEvents(_In_ PVOID Extension, _In_ ULONG EventMask, _In_ BOOLEAN Enable);
VOID NTAPI SdhciClearEvents(_In_ PVOID Extension, _In_ ULONG EventMask);
VOID NTAPI SdhciRequestDpc(_In_ PVOID Extension, _In_ PSDPORT_REQUEST Request, _In_ ULONG Events, _In_ ULONG Errors);

VOID NTAPI SdhciFillInitializationData(_Out_ PSDPORT_INITIALIZATION_DATA InitData);

#endif /* _SDHCI_H_ */
