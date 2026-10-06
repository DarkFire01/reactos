/*
 * spb.h
 *
 * Simple Peripheral Bus IOCTL interface, for use by client drivers.
 *
 * This file is part of the ReactOS DDK package.
 *
 * Contributors:
 *   Created by Justin Miller <justinmiller100@gmail.com>
 *
 * THIS SOFTWARE IS NOT COPYRIGHTED
 *
 * This source code is offered for use in the public domain. You may
 * use, modify or distribute it freely.
 *
 * This code is distributed in the hope that it will be useful but
 * WITHOUT ANY WARRANTY. ALL WARRANTIES, EXPRESS OR IMPLIED ARE HEREBY
 * DISCLAIMED. This includes but is not limited to warranties of
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
 */

#ifndef _SPB_H_
#define _SPB_H_

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A peripheral on an I2C/SPI/UART bus reaches its controller by opening the
 * resource-hub path built from the CmResourceTypeConnection descriptor in its
 * own resources (see <reshub.h>), then issuing these IOCTLs on the resulting
 * file. The controller driver hosts SpbCx, which turns them into the callbacks
 * in <spbcx.h>.
 */

#define SPB_TARGET_NAME_PREFIX L"\\SPB\\"

typedef enum SpbIoctl
{
  IOCTL_SPB_LOCK_CONTROLLER = CTL_CODE(FILE_DEVICE_CONTROLLER,
                                       0x600,
                                       METHOD_BUFFERED,
                                       FILE_ANY_ACCESS),
  IOCTL_SPB_UNLOCK_CONTROLLER = CTL_CODE(FILE_DEVICE_CONTROLLER,
                                         0x601,
                                         METHOD_BUFFERED,
                                         FILE_ANY_ACCESS),
  IOCTL_SPB_EXECUTE_SEQUENCE = CTL_CODE(FILE_DEVICE_CONTROLLER,
                                        0x602,
                                        METHOD_BUFFERED,
                                        FILE_ANY_ACCESS),
  IOCTL_SPB_LOCK_CONNECTION = CTL_CODE(FILE_DEVICE_CONTROLLER,
                                       0x603,
                                       METHOD_BUFFERED,
                                       FILE_ANY_ACCESS),
  IOCTL_SPB_UNLOCK_CONNECTION = CTL_CODE(FILE_DEVICE_CONTROLLER,
                                         0x604,
                                         METHOD_BUFFERED,
                                         FILE_ANY_ACCESS),
  IOCTL_SPB_FULL_DUPLEX = CTL_CODE(FILE_DEVICE_CONTROLLER,
                                   0x605,
                                   METHOD_BUFFERED,
                                   FILE_ANY_ACCESS),
  IOCTL_SPB_MULTI_SPI_TRANSFER = CTL_CODE(FILE_DEVICE_CONTROLLER,
                                          0x606,
                                          METHOD_BUFFERED,
                                          FILE_ANY_ACCESS)
} SpbIoctl, *PSpbIoctl;

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

#ifndef _NTDDK_
#ifndef PMDL
#define PMDL PVOID
#endif
#endif

typedef struct SPB_TRANSFER_BUFFER
{
  SPB_TRANSFER_BUFFER_FORMAT Format;
  __C89_NAMELESS union {
    SPB_TRANSFER_BUFFER_LIST_ENTRY Simple;
    struct {
      PSPB_TRANSFER_BUFFER_LIST_ENTRY List;
      ULONG ListCe;
    } BufferList;
    PMDL Mdl;
  } DUMMYUNIONNAME;
} SPB_TRANSFER_BUFFER, *PSPB_TRANSFER_BUFFER;

typedef struct SPB_TRANSFER_LIST_ENTRY
{
  SPB_TRANSFER_DIRECTION Direction;
  ULONG DelayInUs;
  SPB_TRANSFER_BUFFER Buffer;
} SPB_TRANSFER_LIST_ENTRY, *PSPB_TRANSFER_LIST_ENTRY;

FORCEINLINE SPB_TRANSFER_LIST_ENTRY
SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
  _In_ SPB_TRANSFER_DIRECTION Direction,
  _In_ ULONG DelayInUs,
  _Pre_writable_byte_size_(BufferCb) PVOID Buffer,
  _In_ ULONG BufferCb)
{
  SPB_TRANSFER_LIST_ENTRY entry;

  entry.Direction = Direction;
  entry.DelayInUs = DelayInUs;
  entry.Buffer.Format = SpbTransferBufferFormatSimple;
  entry.Buffer.Simple.Buffer = Buffer;
  entry.Buffer.Simple.BufferCb = BufferCb;

  return entry;
}

FORCEINLINE SPB_TRANSFER_LIST_ENTRY
SPB_TRANSFER_LIST_ENTRY_INIT_NON_PAGED(
  _In_ SPB_TRANSFER_DIRECTION Direction,
  _In_ ULONG DelayInUs,
  _Pre_writable_byte_size_(BufferCb) PVOID Buffer,
  _In_ ULONG BufferCb)
{
  SPB_TRANSFER_LIST_ENTRY entry;

  entry.Direction = Direction;
  entry.DelayInUs = DelayInUs;
  entry.Buffer.Format = SpbTransferBufferFormatSimpleNonPaged;
  entry.Buffer.Simple.Buffer = Buffer;
  entry.Buffer.Simple.BufferCb = BufferCb;

  return entry;
}

FORCEINLINE SPB_TRANSFER_LIST_ENTRY
SPB_TRANSFER_LIST_ENTRY_INIT_MDL(
  _In_ SPB_TRANSFER_DIRECTION Direction,
  _In_ ULONG DelayInUs,
  _In_ PMDL Mdl)
{
  SPB_TRANSFER_LIST_ENTRY entry;

  entry.Direction = Direction;
  entry.DelayInUs = DelayInUs;
  entry.Buffer.Format = SpbTransferBufferFormatMdl;
  entry.Buffer.Mdl = Mdl;

  return entry;
}

FORCEINLINE SPB_TRANSFER_LIST_ENTRY
SPB_TRANSFER_LIST_ENTRY_INIT_BUFFER_LIST(
  _In_ SPB_TRANSFER_DIRECTION Direction,
  _In_ ULONG DelayInUs,
  _In_ SPB_TRANSFER_BUFFER_LIST_ENTRY BufferList[],
  _In_ ULONG BufferListCe)
{
  SPB_TRANSFER_LIST_ENTRY entry;

  entry.Direction = Direction;
  entry.DelayInUs = DelayInUs;
  entry.Buffer.Format = SpbTransferBufferFormatList;
  entry.Buffer.BufferList.List = BufferList;
  entry.Buffer.BufferList.ListCe = BufferListCe;

  return entry;
}

typedef struct SPB_TRANSFER_LIST
{
  _Field_range_(==, sizeof(SPB_TRANSFER_LIST))
  ULONG Size;
  ULONG Reserved;
  ULONG TransferCount;
  _Field_size_(TransferCount) SPB_TRANSFER_LIST_ENTRY Transfers[1];
} SPB_TRANSFER_LIST, *PSPB_TRANSFER_LIST;

#define SPB_TRANSFER_LIST_AND_ENTRIES(n) struct {   \
    SPB_TRANSFER_LIST List;                         \
    SPB_TRANSFER_LIST_ENTRY ExtraTransfers[(n) - 1]; \
}

FORCEINLINE VOID
SPB_TRANSFER_LIST_INIT(
  _Out_ SPB_TRANSFER_LIST *TransferList,
  _In_ ULONG TransferCount)
{
  RtlZeroMemory(TransferList,
                (sizeof(SPB_TRANSFER_LIST) +
                 (sizeof(SPB_TRANSFER_LIST_ENTRY) * (TransferCount - 1))));

  TransferList->Size = sizeof(SPB_TRANSFER_LIST);
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

typedef struct SPB_MULTI_SPI_WRITE_TRANSFER
{
  SPB_MULTI_SPI_TRANSFER SpiTransfer;
} SPB_MULTI_SPI_WRITE_TRANSFER, *PSPB_MULTI_SPI_WRITE_TRANSFER;

typedef struct SPB_MULTI_SPI_READ_TRANSFER
{
  SPB_MULTI_SPI_TRANSFER SpiTransfer;
  SPB_TRANSFER_LIST_ENTRY ExtraTransfer;
} SPB_MULTI_SPI_READ_TRANSFER, *PSPB_MULTI_SPI_READ_TRANSFER;

C_ASSERT(sizeof(SPB_MULTI_SPI_READ_TRANSFER) ==
         sizeof(SPB_MULTI_SPI_TRANSFER) + sizeof(SPB_TRANSFER_LIST_ENTRY));

FORCEINLINE VOID
SPB_MULTI_SPI_TRANSFER_INIT(
  _Out_ SPB_MULTI_SPI_TRANSFER *SpiTransfer,
  _In_ SPB_MULTI_SPI_TRANSFER_MODE Mode,
  _In_ ULONG TransferPhaseCount,
  _In_ ULONG WritePhaseSingleSpiByteCount,
  _In_ ULONG WaitCycleByteCount)
{
  RtlZeroMemory(SpiTransfer,
                sizeof(SPB_MULTI_SPI_TRANSFER) +
                (sizeof(SPB_TRANSFER_LIST_ENTRY) * (TransferPhaseCount - 1)));

  SpiTransfer->Header.Size = sizeof(SPB_MULTI_SPI_TRANSFER);
  SpiTransfer->Header.Mode = Mode;
  SpiTransfer->Header.WritePhaseSingleSpiByteCount = WritePhaseSingleSpiByteCount;
  SpiTransfer->Header.WaitCycleByteCount = WaitCycleByteCount;
  SpiTransfer->TransferPhaseCount = TransferPhaseCount;
}

FORCEINLINE VOID
SPB_MULTI_SPI_WRITE_TRANSFER_INIT(
  _Out_ SPB_MULTI_SPI_WRITE_TRANSFER *SpiTransfer,
  _In_ SPB_MULTI_SPI_TRANSFER_MODE Mode,
  _In_ ULONG WritePhaseSingleSpiByteCount,
  _In_ ULONG WaitCycleByteCount)
{
  SPB_MULTI_SPI_TRANSFER_INIT(&SpiTransfer->SpiTransfer,
                              Mode,
                              1,
                              WritePhaseSingleSpiByteCount,
                              WaitCycleByteCount);
}

FORCEINLINE VOID
SPB_MULTI_SPI_READ_TRANSFER_INIT(
  _Out_ SPB_MULTI_SPI_READ_TRANSFER *SpiTransfer,
  _In_ SPB_MULTI_SPI_TRANSFER_MODE Mode,
  _In_ ULONG WritePhaseSingleSpiByteCount,
  _In_ ULONG WaitCycleByteCount)
{
  SPB_MULTI_SPI_TRANSFER_INIT(&SpiTransfer->SpiTransfer,
                              Mode,
                              2,
                              WritePhaseSingleSpiByteCount,
                              WaitCycleByteCount);
}

#ifdef __cplusplus
}
#endif

#endif /* _SPB_H_ */
