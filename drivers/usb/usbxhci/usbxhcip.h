/*
 * PROJECT:         ReactOS xHCI Driver
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         private xHCI functions and definitions
 * COPYRIGHT:       Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

#include "usbxhci.h"

/* Private Functions ******************************************************************************/

MPSTATUS
NTAPI
PXHCI_ControllerWorkTest(IN PXHCI_EXTENSION XhciExtension,
                         IN PXHCI_HC_RESOURCES HcResourcesVA,
                         IN PVOID resourcesStartPA);

VOID
NTAPI
PXHCI_PortStatusChange(IN PXHCI_EXTENSION XhciExtension, 
                       IN ULONG PortID);

VOID  
NTAPI
PXHCI_AssignSlot(IN PXHCI_EXTENSION xhciExtension, ULONG PortID);

VOID
NTAPI
PXHCI_InitSlot(IN PXHCI_EXTENSION xhciExtension, ULONG PortID, ULONG SlotID);

VOID
NTAPI
XHCI_Write64bitReg(IN PULONG BaseAddr,
                   IN ULONGLONG Data);


/* Transfer type functions ************************************************************************/

MPSTATUS
NTAPI
PXHCI_InitTransferBulk(PVOID xhciExtension);

MPSTATUS
NTAPI
PXHCI_InitTransferInterrupt(PVOID xhciExtension);

MPSTATUS
NTAPI
PXHCI_InitTransferIso(PVOID xhciExtension);

MPSTATUS
NTAPI
PXHCI_InitTransferControl(PVOID xhciExtension);

/* endpoint type functions ************************************************************************/

MPSTATUS
NTAPI
XHCI_OpenIsoEndpoint(IN PXHCI_EXTENSION XhciExtension,
                     IN PUSBPORT_ENDPOINT_PROPERTIES EndpointProperties,
                     IN PXHCI_ENDPOINT  XhciEndpoint);

MPSTATUS
NTAPI
XHCI_OpenControlEndpoint(IN PXHCI_EXTENSION XhciExtension,
                         IN PUSBPORT_ENDPOINT_PROPERTIES EndpointProperties,
                         IN PXHCI_ENDPOINT  XhciEndpoint);
MPSTATUS
NTAPI
XHCI_OpenBulkEndpoint(IN PXHCI_EXTENSION XhciExtension,
                      IN PUSBPORT_ENDPOINT_PROPERTIES EndpointProperties,
                      IN PXHCI_ENDPOINT  XhciEndpoint);
MPSTATUS
NTAPI
XHCI_OpenInterruptEndpoint(IN PXHCI_EXTENSION XhciExtension,
                           IN PUSBPORT_ENDPOINT_PROPERTIES EndpointProperties,
                           IN PXHCI_ENDPOINT  XhciEndpoint);

XHCI_ENDPOINT
NTAPI
PXHCI_CreateCmdTrb (_In_ PXHCI_EXTENSION XhciExtension,
                    _In_ PXHCI_TRB       CmdTrb
  );

VOID
NTAPI
PXHCI_ExecTransfer (_In_  PXHCI_EXTENSION XhciExtension,
                 _In_  BOOLEAN            CmdTransfer,
                 _In_  XHCI_ENDPOINT YourLocalEndpoint);