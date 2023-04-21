/*
 * PROJECT:         ReactOS xHCI Driver
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Resource definitions
 * COPYRIGHT:       Copyright 2017 Rama Teja Gampa <ramateja.g@gmail.com>
 */

#pragma once

#include <usb/xhcispec.h>
#include <usbdlib.h>
#define CMD_RING_TRB_NUMBER    0x100
#define TR_RING_TRB_NUMBER     0x100
#define ERST_NUMBER            0x01
#define EVENT_RING_TRB_NUMBER  0x200


#define  BIT0     0x00000001
#define  BIT1     0x00000002
#define  BIT2     0x00000004
#define  BIT3     0x00000008
#define  BIT4     0x00000010
#define  BIT5     0x00000020
#define  BIT6     0x00000040
#define  BIT7     0x00000080
#define  BIT8     0x00000100
#define  BIT9     0x00000200
#define  BIT10    0x00000400
#define  BIT11    0x00000800
#define  BIT12    0x00001000
#define  BIT13    0x00002000
#define  BIT14    0x00004000
#define  BIT15    0x00008000
#define  BIT16    0x00010000
#define  BIT17    0x00020000
#define  BIT18    0x00040000
#define  BIT19    0x00080000
#define  BIT20    0x00100000
#define  BIT21    0x00200000
#define  BIT22    0x00400000
#define  BIT23    0x00800000
#define  BIT24    0x01000000
#define  BIT25    0x02000000
#define  BIT26    0x04000000
#define  BIT27    0x08000000
#define  BIT28    0x10000000
#define  BIT29    0x20000000
#define  BIT30    0x40000000
#define  BIT31    0x80000000
#define  BIT32    0x0000000100000000ULL
#define  BIT33    0x0000000200000000ULL
#define  BIT34    0x0000000400000000ULL
#define  BIT35    0x0000000800000000ULL
#define  BIT36    0x0000001000000000ULL
#define  BIT37    0x0000002000000000ULL
#define  BIT38    0x0000004000000000ULL
#define  BIT39    0x0000008000000000ULL
#define  BIT40    0x0000010000000000ULL
#define  BIT41    0x0000020000000000ULL
#define  BIT42    0x0000040000000000ULL
#define  BIT43    0x0000080000000000ULL
#define  BIT44    0x0000100000000000ULL
#define  BIT45    0x0000200000000000ULL
#define  BIT46    0x0000400000000000ULL
#define  BIT47    0x0000800000000000ULL
#define  BIT48    0x0001000000000000ULL
#define  BIT49    0x0002000000000000ULL
#define  BIT50    0x0004000000000000ULL
#define  BIT51    0x0008000000000000ULL
#define  BIT52    0x0010000000000000ULL
#define  BIT53    0x0020000000000000ULL
#define  BIT54    0x0040000000000000ULL
#define  BIT55    0x0080000000000000ULL
#define  BIT56    0x0100000000000000ULL
#define  BIT57    0x0200000000000000ULL
#define  BIT58    0x0400000000000000ULL
#define  BIT59    0x0800000000000000ULL
#define  BIT60    0x1000000000000000ULL
#define  BIT61    0x2000000000000000ULL
#define  BIT62    0x4000000000000000ULL
#define  BIT63    0x8000000000000000ULL

#include "hardware.h"

/* XHCI Transfer follows USBPORT Transfer */
typedef struct _XHCI_TRANSFER {
  ULONG Reserved;
  PUSBPORT_TRANSFER_PARAMETERS TransferParameters;
  ULONG USBDStatus;
  ULONG TransferLen;
  PXHCI_ENDPOINT XhciEndpoint;
  ULONG PendingTDs;
  ULONG TransferOnAsyncList;
} XHCI_TRANSFER, *PXHCI_TRANSFER;

typedef struct _TRB_TEMPLATE {
  UINT32    Parameter1;

  UINT32    Parameter2;

  UINT32    Status;

  UINT32    CycleBit : 1;
  UINT32    RsvdZ1   : 9;
  UINT32    Type     : 6;
  UINT32    Control  : 16;
} TRB_TEMPLATE;
/* Totally Based off of the fact EHCI exists */
typedef struct _XHCI_HCD_TD {
  /* Hardware*/
  PXHCI_GENERIC_TRB HwTd; /* Can be converted to any TRB type*/
  /* Software */
  ULONG PhysicalAddress;
  ULONG TdFlags;
  struct _XHCI_ENDPOINT * XhciEndpoint;
  struct _XHCI_TRANSFER * XhciTransfer;
  struct _XHCI_HCD_TD * NextHcdTD;
  struct _XHCI_HCD_TD * AltNextHcdTD;
  USB_DEFAULT_PIPE_SETUP_PACKET SetupPacket;
  ULONG LengthThisTD;
  
  LIST_ENTRY DoneLink;
#ifdef _WIN64
  ULONG Pad[31];
#else
  ULONG Pad[40];
#endif
} XHCI_HCD_TD, *PXHCI_HCD_TD;

typedef struct _XHCI_GLOBAL_SLOT_TRACKER
{
    ULONG32 PortIndex[256];
} XHCI_GLOBAL_SLOT_TRACKER, *PXHCI_GLOBAL_SLOT_TRACKER;

/* Roothub Functions ******************************************************************************/

VOID
NTAPI
XHCI_RH_GetRootHubData(
  IN PVOID xhciExtension,
  IN PVOID rootHubData);

MPSTATUS
NTAPI
XHCI_RH_GetStatus(
  IN PVOID xhciExtension,
  IN PUSHORT Status);

MPSTATUS
NTAPI
XHCI_RH_GetPortStatus(
  IN PVOID xhciExtension,
  IN USHORT Port,
  IN PUSB_PORT_STATUS_AND_CHANGE PortStatus);

MPSTATUS
NTAPI
XHCI_RH_GetHubStatus(
  IN PVOID xhciExtension,
  IN PUSB_HUB_STATUS_AND_CHANGE HubStatus);

MPSTATUS
NTAPI
XHCI_RH_SetFeaturePortReset(
  IN PVOID xhciExtension,
  IN USHORT Port);

MPSTATUS
NTAPI
XHCI_RH_SetFeaturePortPower(
  IN PVOID xhciExtension,
  IN USHORT Port);

MPSTATUS
NTAPI
XHCI_RH_SetFeaturePortEnable(
  IN PVOID xhciExtension,
  IN USHORT Port);

MPSTATUS
NTAPI
XHCI_RH_SetFeaturePortSuspend(
  IN PVOID xhciExtension,
  IN USHORT Port);

MPSTATUS
NTAPI
XHCI_RH_ClearFeaturePortEnable(
  IN PVOID xhciExtension,
  IN USHORT Port);

MPSTATUS
NTAPI
XHCI_RH_ClearFeaturePortPower(
  IN PVOID xhciExtension,
  IN USHORT Port);

MPSTATUS
NTAPI
XHCI_RH_ClearFeaturePortSuspend(
  IN PVOID xhciExtension,
  IN USHORT Port);

MPSTATUS
NTAPI
XHCI_RH_ClearFeaturePortEnableChange(
  IN PVOID xhciExtension,
  IN USHORT Port);

MPSTATUS
NTAPI
XHCI_RH_ClearFeaturePortConnectChange(
  IN PVOID xhciExtension,
  IN USHORT Port);

MPSTATUS
NTAPI
XHCI_RH_ClearFeaturePortResetChange(
  IN PVOID xhciExtension,
  IN USHORT Port);

MPSTATUS
NTAPI
XHCI_RH_ClearFeaturePortSuspendChange(
  IN PVOID xhciExtension,
  IN USHORT Port);

MPSTATUS
NTAPI
XHCI_RH_ClearFeaturePortOvercurrentChange(
  IN PVOID xhciExtension,
  IN USHORT Port);

VOID
NTAPI
XHCI_RH_DisableIrq(
  IN PVOID xhciExtension);

VOID
NTAPI
XHCI_RH_EnableIrq(
  IN PVOID xhciExtension);

MPSTATUS
NTAPI
XHCI_SendCommand(
    IN XHCI_TRB CommandTRB,
    IN PXHCI_EXTENSION XhciExtension);

MPSTATUS
NTAPI
XHCI_ProcessEvent(
    IN PXHCI_EXTENSION XhciExtension);

MPSTATUS
NTAPI
PXHCI_TEST(PXHCI_EXTENSION XhciExtension, PXHCI_ENDPOINT XhciEndpoint);

MPSTATUS
NTAPI
PXHCI_GenerateTransferTrb(_In_ PXHCI_EXTENSION XhciExtension,
                          _In_ PXHCI_ENDPOINT XhciEndpoint);

MPSTATUS
NTAPI
PXHCI_CheckTrbResult(_In_    PXHCI_EXTENSION XhciExtension,
                     _Inout_ PXHCI_EVENT_TRB eventTRB);


                     MPSTATUS
NTAPI
PXHCI_SyncTrsRing (_In_ PXHCI_EXTENSION XhciExtension,
                   _In_ PXHCI_TRANSFER_RING      TrsRing);