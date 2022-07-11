/*
 * PROJECT:         ReactOS xHCI Driver
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         roothub functions of xHCI
 * COPYRIGHT:       2022 Justin Miller <justinmiller100@gmail.com>
 */

#include "usbxhci.h"
#include "dbg_xhci.h"
#define NDEBUG
#include <debug.h>

VOID
NTAPI
XHCI_RH_GetRootHubData(IN PVOID xhciExtension,
                       IN PVOID rootHubData)
{
    DPRINT1("XHCI_RH_GetRootHubData: function initiated\n");
}

MPSTATUS
NTAPI
XHCI_RH_GetStatus(IN PVOID xhciExtension,
                  IN PUSHORT Status)
{
    DPRINT1("XHCI_RH_GetStatus: function initiated\n");
    *Status = USB_GETSTATUS_SELF_POWERED;
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_RH_GetPortStatus(IN PVOID xhciExtension,
                      IN USHORT Port,
                      IN PUSB_PORT_STATUS_AND_CHANGE PortStatus)
{
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_RH_GetHubStatus(IN PVOID xhciExtension,
                     IN PUSB_HUB_STATUS_AND_CHANGE HubStatus)
{
    DPRINT("XHCI_RH_GetHubStatus: function initiated\n"); //removed to reduce windbg output
    HubStatus->AsUlong32 = 0;
    return MP_STATUS_SUCCESS;
}

VOID
NTAPI
XHCI_RH_FinishReset(IN PVOID xhciExtension,
                    IN PUSHORT Port)
{
    DPRINT1("XHCI_RH_FinishReset: function initiated\n");
}

ULONG
NTAPI
XHCI_RH_PortResetComplete(IN PVOID xhciExtension,
                          IN PUSHORT Port)
{
    DPRINT1("XHCI_RH_PortResetComplete: function initiated\n");
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_RH_SetFeaturePortReset(IN PVOID xhciExtension,
                            IN USHORT Port)
{
    DPRINT1("XHCI_RH_SetFeaturePortReset: function initiated\n");
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_RH_SetFeaturePortPower(IN PVOID xhciExtension,
                            IN USHORT Port)
{
    DPRINT1("XHCI_RH_SetFeaturePortPower: function initiated\n");
    return MP_STATUS_SUCCESS;
}
MPSTATUS
NTAPI
XHCI_RH_SetFeaturePortEnable(IN PVOID xhciExtension,
                             IN USHORT Port)
{
    DPRINT_RH("XHCI_RH_SetFeaturePortEnable: Not supported\n");
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_RH_SetFeaturePortSuspend(IN PVOID xhciExtension,
                              IN USHORT Port)
{
    DPRINT1("XHCI_RH_SetFeaturePortSuspend: function initiated\n");
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_RH_ClearFeaturePortEnable(IN PVOID xhciExtension,
                               IN USHORT Port)
{
    DPRINT1("XHCI_RH_ClearFeaturePortEnable: function initiated\n");
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_RH_ClearFeaturePortPower(IN PVOID xhciExtension,
                              IN USHORT Port)
{
    DPRINT1("XHCI_RH_ClearFeaturePortPower: function initiated\n");
    return MP_STATUS_SUCCESS;
}

VOID
NTAPI
XHCI_RH_PortResumeComplete(IN PULONG xhciExtension,
                           IN PUSHORT Port)
{
   DPRINT1("XHCI_RH_PortResumeComplete: function initiated\n");
}

MPSTATUS
NTAPI
XHCI_RH_ClearFeaturePortSuspend(IN PVOID xhciExtension,
                                IN USHORT Port)
{
    DPRINT1("XHCI_RH_ClearFeaturePortSuspend: function initiated\n");
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_RH_ClearFeaturePortEnableChange(IN PVOID xhciExtension,
                                     IN USHORT Port)
{
    DPRINT1("XHCI_RH_ClearFeaturePortEnableChange: function initiated\n");
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_RH_ClearFeaturePortConnectChange(IN PVOID xhciExtension,
                                      IN USHORT Port)
{
    DPRINT1("XHCI_RH_ClearFeaturePortConnectChange: function initiated\n");
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_RH_ClearFeaturePortResetChange(IN PVOID xhciExtension,
                                    IN USHORT Port)
{
    DPRINT1("XHCI_RH_ClearFeaturePortResetChange: function initiated\n");
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_RH_ClearFeaturePortSuspendChange(IN PVOID xhciExtension,
                                      IN USHORT Port)
{
    DPRINT1("XHCI_RH_ClearFeaturePortSuspendChange: function initiated\n");
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_RH_ClearFeaturePortOvercurrentChange(IN PVOID xhciExtension,
                                          IN USHORT Port)
{
    DPRINT1("XHCI_RH_ClearFeaturePortOvercurrentChange: function initiated\n");
    return MP_STATUS_SUCCESS;
}

VOID
NTAPI
XHCI_RH_DisableIrq(IN PVOID xhciExtension)
{
    DPRINT1("XHCI_RH_DisableIrq: function initiated\n");
}

VOID
NTAPI
XHCI_RH_EnableIrq(IN PVOID xhciExtension)
{
    DPRINT1("XHCI_RH_EnableIrq: function initiated\n");
}
