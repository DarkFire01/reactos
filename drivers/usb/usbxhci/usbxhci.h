/*
 * PROJECT:         ReactOS xHCI Driver
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Resource definitions
 * COPYRIGHT:       2022 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

#include <ntddk.h>
#include <windef.h>
#include <hubbusif.h>
#include <usbbusif.h>
#include <ntdef.h>
#include <drivers/usbport/usbmport.h>
#include "usbxhci.h"
#include "xhcispec.h"
#include "xhcitrb.h"
#include "hardware.h"
#include <debug.h>

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
