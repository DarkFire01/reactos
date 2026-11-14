/*
 * PROJECT:     ReactOS DDK
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Simple Peripheral Bus I/O control interface
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define SPB_TARGET_NAME_PREFIX L"\\SPB\\"

/* Controls a peripheral driver sends to its SPB target */
#define IOCTL_SPB_LOCK_CONTROLLER    CTL_CODE(FILE_DEVICE_CONTROLLER, 0x600, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_SPB_UNLOCK_CONTROLLER  CTL_CODE(FILE_DEVICE_CONTROLLER, 0x601, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_SPB_EXECUTE_SEQUENCE   CTL_CODE(FILE_DEVICE_CONTROLLER, 0x602, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_SPB_LOCK_CONNECTION    CTL_CODE(FILE_DEVICE_CONTROLLER, 0x603, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_SPB_UNLOCK_CONNECTION  CTL_CODE(FILE_DEVICE_CONTROLLER, 0x604, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_SPB_FULL_DUPLEX        CTL_CODE(FILE_DEVICE_CONTROLLER, 0x605, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_SPB_MULTI_SPI_TRANSFER CTL_CODE(FILE_DEVICE_CONTROLLER, 0x606, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef enum SPB_TRANSFER_DIRECTION
{
    SpbTransferDirectionNone,
    SpbTransferDirectionFromDevice,
    SpbTransferDirectionToDevice,
    SpbTransferDirectionMax
} SPB_TRANSFER_DIRECTION, *PSPB_TRANSFER_DIRECTION;

typedef enum SPB_TRANSFER_BUFFER_FORMAT
{
    SpbTransferBufferFormatInvalid,
    SpbTransferBufferFormatSimple,
    SpbTransferBufferFormatList,
    SpbTransferBufferFormatSimpleNonPaged,
    SpbTransferBufferFormatMdl,
    SpbTransferBufferFormatMax
} SPB_TRANSFER_BUFFER_FORMAT, *PSPB_TRANSFER_BUFFER_FORMAT;

typedef struct SPB_TRANSFER_BUFFER_LIST_ENTRY
{
    PVOID Buffer;
    ULONG BufferCb;
} SPB_TRANSFER_BUFFER_LIST_ENTRY, *PSPB_TRANSFER_BUFFER_LIST_ENTRY;

typedef struct SPB_TRANSFER_BUFFER
{
    SPB_TRANSFER_BUFFER_FORMAT Format;
    union
    {
        SPB_TRANSFER_BUFFER_LIST_ENTRY Simple;
        struct
        {
            PSPB_TRANSFER_BUFFER_LIST_ENTRY List;
            ULONG ListCe;
        } BufferList;
        PMDL Mdl;
    };
} SPB_TRANSFER_BUFFER, *PSPB_TRANSFER_BUFFER;

typedef struct SPB_TRANSFER_LIST_ENTRY
{
    SPB_TRANSFER_DIRECTION Direction;
    ULONG DelayInUs;
    SPB_TRANSFER_BUFFER Buffer;
} SPB_TRANSFER_LIST_ENTRY, *PSPB_TRANSFER_LIST_ENTRY;

/**
 * @brief Describes one transfer over a plain buffer.
 */
FORCEINLINE
SPB_TRANSFER_LIST_ENTRY
SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
    _In_ SPB_TRANSFER_DIRECTION Direction,
    _In_ ULONG DelayInUs,
    _In_reads_bytes_(BufferCb) PVOID Buffer,
    _In_ ULONG BufferCb)
{
    SPB_TRANSFER_LIST_ENTRY Entry;

    Entry.Direction = Direction;
    Entry.DelayInUs = DelayInUs;
    Entry.Buffer.Format = SpbTransferBufferFormatSimple;
    Entry.Buffer.Simple.Buffer = Buffer;
    Entry.Buffer.Simple.BufferCb = BufferCb;
    return Entry;
}

/**
 * @brief Describes one transfer over a buffer the caller keeps resident.
 */
FORCEINLINE
SPB_TRANSFER_LIST_ENTRY
SPB_TRANSFER_LIST_ENTRY_INIT_NON_PAGED(
    _In_ SPB_TRANSFER_DIRECTION Direction,
    _In_ ULONG DelayInUs,
    _In_reads_bytes_(BufferCb) PVOID Buffer,
    _In_ ULONG BufferCb)
{
    SPB_TRANSFER_LIST_ENTRY Entry;

    Entry = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(Direction, DelayInUs, Buffer, BufferCb);
    Entry.Buffer.Format = SpbTransferBufferFormatSimpleNonPaged;
    return Entry;
}

/**
 * @brief Describes one transfer over an MDL.
 */
FORCEINLINE
SPB_TRANSFER_LIST_ENTRY
SPB_TRANSFER_LIST_ENTRY_INIT_MDL(
    _In_ SPB_TRANSFER_DIRECTION Direction,
    _In_ ULONG DelayInUs,
    _In_ PMDL Mdl)
{
    SPB_TRANSFER_LIST_ENTRY Entry;

    Entry.Direction = Direction;
    Entry.DelayInUs = DelayInUs;
    Entry.Buffer.Format = SpbTransferBufferFormatMdl;
    Entry.Buffer.Mdl = Mdl;
    return Entry;
}

/**
 * @brief Describes one transfer scattered over several buffers.
 */
FORCEINLINE
SPB_TRANSFER_LIST_ENTRY
SPB_TRANSFER_LIST_ENTRY_INIT_BUFFER_LIST(
    _In_ SPB_TRANSFER_DIRECTION Direction,
    _In_ ULONG DelayInUs,
    _In_reads_(BufferListCe) SPB_TRANSFER_BUFFER_LIST_ENTRY BufferList[],
    _In_ ULONG BufferListCe)
{
    SPB_TRANSFER_LIST_ENTRY Entry;

    Entry.Direction = Direction;
    Entry.DelayInUs = DelayInUs;
    Entry.Buffer.Format = SpbTransferBufferFormatList;
    Entry.Buffer.BufferList.List = BufferList;
    Entry.Buffer.BufferList.ListCe = BufferListCe;
    return Entry;
}

typedef struct SPB_TRANSFER_LIST
{
    ULONG Size;
    ULONG Reserved;
    ULONG TransferCount;
    _Field_size_(TransferCount) SPB_TRANSFER_LIST_ENTRY Transfers[1];
} SPB_TRANSFER_LIST, *PSPB_TRANSFER_LIST;

/* A transfer list with room for Count entries */
#define SPB_TRANSFER_LIST_AND_ENTRIES(Count) \
    struct { SPB_TRANSFER_LIST List; SPB_TRANSFER_LIST_ENTRY ExtraTransfers[(Count) - 1]; }

/**
 * @brief Zeroes a transfer list sized for TransferCount entries and fills its header.
 */
FORCEINLINE
VOID
SPB_TRANSFER_LIST_INIT(
    _Out_ SPB_TRANSFER_LIST *TransferList,
    _In_ ULONG TransferCount)
{
    RtlZeroMemory(TransferList,
                  FIELD_OFFSET(SPB_TRANSFER_LIST, Transfers) +
                  TransferCount * sizeof(SPB_TRANSFER_LIST_ENTRY));
    TransferList->Size = sizeof(*TransferList);
    TransferList->TransferCount = TransferCount;
}

typedef enum SPB_MULTI_SPI_TRANSFER_MODE
{
    SpbMultiSpiTransferModeInvalid,
    SpbMultiSpiTransferModeDualSpi,
    SpbMultiSpiTransferModeQuadSpi,
    SpbMultiSpiTransferModeMax
} SPB_MULTI_SPI_TRANSFER_MODE, *PSPB_MULTI_SPI_TRANSFER_MODE;

typedef struct SPB_MULTI_SPI_TRANSFER_HEADER
{
    ULONG Size;
    SPB_MULTI_SPI_TRANSFER_MODE Mode;
    ULONG WritePhaseSingleSpiByteCount;
    ULONG WaitCycleByteCount;
} SPB_MULTI_SPI_TRANSFER_HEADER, *PSPB_MULTI_SPI_TRANSFER_HEADER;

typedef struct SPB_MULTI_SPI_TRANSFER
{
    SPB_MULTI_SPI_TRANSFER_HEADER Header;
    ULONG TransferPhaseCount;
    _Field_size_(TransferPhaseCount) SPB_TRANSFER_LIST_ENTRY TransferPhases[1];
} SPB_MULTI_SPI_TRANSFER, *PSPB_MULTI_SPI_TRANSFER;

#ifdef __cplusplus
}
#endif
