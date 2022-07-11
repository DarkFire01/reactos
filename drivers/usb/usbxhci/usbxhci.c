/*
 * PROJECT:         ReactOS xHCI Driver
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         main functions of xHCI
 * COPYRIGHT:       2022 Justin Miller <justinmiller100@gmail.com>
*/

#include "usbxhci.h"
#define NDEBUG
#include <debug.h>

/* Globals ****************************************************************************************/

USBPORT_REGISTRATION_PACKET RegPacket;

/* Public Functions *******************************************************************************/

VOID
NTAPI
XHCI_Write64bitReg(IN PULONG BaseAddr,
                   IN ULONGLONG Data)
{
    //WRITE_REGISTER_ULONG(BaseAddr, Data);
    //WRITE_REGISTER_ULONG(BaseAddr + 1, Data >> 32);
}

MPSTATUS
NTAPI
XHCI_OpenEndpoint(IN PVOID xhciExtension,
                  IN PUSBPORT_ENDPOINT_PROPERTIES  endpointParameters,
                  IN PVOID xhciEndpoint)
{
    DPRINT1("XHCI_OpenEndpoint: function initiated\n");
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_ReopenEndpoint(IN PVOID xhciExtension,
                    IN PUSBPORT_ENDPOINT_PROPERTIES  endpointParameters,
                    IN PVOID  xhciEndpoint)
{
    DPRINT1("XHCI_ReopenEndpoint: function initiated\n");
    return MP_STATUS_SUCCESS;
}

VOID
NTAPI
XHCI_QueryEndpointRequirements(IN PVOID xhciExtension,
                               IN PUSBPORT_ENDPOINT_PROPERTIES  endpointParameters,
                               IN PUSBPORT_ENDPOINT_REQUIREMENTS EndpointRequirements)
{
    DPRINT1("XHCI_QueryEndpointRequirements: function initiated\n");
}

VOID
NTAPI
XHCI_CloseEndpoint(IN PVOID xhciExtension,
                   IN PVOID xhciEndpoint,
                   IN BOOLEAN IsDoDisablePeriodic)
{
    DPRINT1("XHCI_CloseEndpoint: UNIMPLEMENTED. FIXME\n");
}

MPSTATUS
NTAPI
XHCI_ProcessEvent (IN PXHCI_EXTENSION XhciExtension)
{
    DPRINT("XHCI_ProcessEvent: Function initiated\n");
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_SendCommand(IN XHCI_TRB CommandTRB,
                 IN PXHCI_EXTENSION XhciExtension)
{
    DPRINT("XHCI_SendCommand: Function initiated\n");
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_InitializeResources(IN PXHCI_EXTENSION XhciExtension,
                         IN ULONG_PTR resourcesStartVA,
                         IN ULONG  resourcesStartPA)
{
    DPRINT("XHCI_InitializeResources: Function initiated\n");
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_InitializeHardware(IN PXHCI_EXTENSION XhciExtension)
{
    DPRINT1("XHCI_InitializeHardware: function initiated\n");
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_StartController(IN PVOID xhciExtension,
                     IN PUSBPORT_RESOURCES Resources)
{
    DPRINT1("XHCI_StartController: function initiated\n");
    return MP_STATUS_SUCCESS;
}


VOID
NTAPI
XHCI_StopController(IN PVOID xhciExtension,
                    IN BOOLEAN IsDoDisableInterrupts)
{
    DPRINT1("XHCI_StopController: Function initiated. \n");
}

VOID
NTAPI
XHCI_SuspendController(IN PVOID xhciExtension)
{

    PXHCI_EXTENSION XhciExtension;
    DPRINT1("XHCI_SuspendController: function initiated\n");
    XhciExtension = (PXHCI_EXTENSION)xhciExtension;

    XhciExtension->Flags |= XHCI_FLAGS_CONTROLLER_SUSPEND;

}

MPSTATUS
NTAPI
XHCI_ResumeController(IN PVOID xhciExtension)
{
    DPRINT1("XHCI_ResumeController: function initiated\n");
    return MP_STATUS_SUCCESS;
}

BOOLEAN
NTAPI
XHCI_HardwarePresent(IN PXHCI_EXTENSION xhciExtension,
                     IN BOOLEAN IsInvalidateController)
{
    DPRINT1("XHCI_HardwarePresent: function initiated\n");
    return TRUE;
}

BOOLEAN
NTAPI
XHCI_InterruptService(IN PVOID xhciExtension)
{
    DPRINT1("XHCI_InterruptService: Succesful Interupt\n");
    return FALSE;
}

VOID
NTAPI
XHCI_InterruptDpc(IN PVOID xhciExtension,
                  IN BOOLEAN IsDoEnableInterrupts)
{
    DPRINT1("XHCI_InterruptDpc: function initiated\n");
    XHCI_ProcessEvent(xhciExtension);
}

MPSTATUS
NTAPI
XHCI_SubmitTransfer(IN PVOID xhciExtension,
                    IN PVOID xhciEndpoint,
                    IN PUSBPORT_TRANSFER_PARAMETERS  transferParameters,
                    IN PVOID xhciTransfer,
                    IN PUSBPORT_SCATTER_GATHER_LIST  sgList)
{
    DPRINT1("XHCI_SubmitTransfer: function initiated\n");
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_SubmitIsoTransfer(IN PVOID xhciExtension,
                       IN PVOID xhciEndpoint,
                       IN PUSBPORT_TRANSFER_PARAMETERS transferParameters,
                       IN PVOID xhciTransfer,
                       IN PVOID isoParameters)
{
    DPRINT1("XHCI_SubmitIsoTransfer: UNIMPLEMENTED. FIXME\n");
    return MP_STATUS_SUCCESS;
}

VOID
NTAPI
XHCI_AbortIsoTransfer(IN PXHCI_EXTENSION xhciExtension,
                      IN PXHCI_ENDPOINT xhciEndpoint,
                      IN PXHCI_TRANSFER xhciTransfer)
{
    DPRINT1("XHCI_AbortIsoTransfer: UNIMPLEMENTED. FIXME\n");
}

VOID
NTAPI
XHCI_AbortAsyncTransfer(IN PXHCI_EXTENSION xhciExtension,
                        IN PXHCI_ENDPOINT xhciEndpoint,
                        IN PXHCI_TRANSFER xhciTransfer)
{
    DPRINT1("XHCI_AbortAsyncTransfer: function initiated\n");
}

VOID
NTAPI
XHCI_AbortTransfer(IN PVOID xhciExtension,
                   IN PVOID xhciEndpoint,
                   IN PVOID xhciTransfer,
                   IN PULONG CompletedLength)
{
    DPRINT1("XHCI_AbortTransfer: function initiated\n");
}

ULONG
NTAPI
XHCI_GetEndpointState(IN PVOID xhciExtension,
                      IN PVOID xhciEndpoint)
{
    DPRINT1("XHCI_GetEndpointState: function initiated\n");
    PXHCI_ENDPOINT XhciEndpoint = xhciEndpoint;
    return XhciEndpoint->EndpointState;
}

VOID
NTAPI
XHCI_SetEndpointState(IN PVOID xhciExtension,
                      IN PVOID xhciEndpoint,
                      IN ULONG EndpointState)
{
    DPRINT1("XHCI_SetEndpointState: function initiated\n");
    PXHCI_ENDPOINT XhciEndpoint = xhciEndpoint;
    XhciEndpoint->EndpointState = EndpointState;
}

VOID
NTAPI
XHCI_PollEndpoint(IN PVOID xhciExtension,
                  IN PVOID xhciEndpoint)
{
    DPRINT1("XHCI_PollEndpoint: function initiated\n");
}

VOID
NTAPI
XHCI_CheckController(IN PVOID xhciExtension)
{
    DPRINT("XHCI_CheckController: function initiated\n");
}

ULONG
NTAPI
XHCI_Get32BitFrameNumber(IN PVOID xhciExtension)
{
    return 0;
}

VOID
NTAPI
XHCI_InterruptNextSOF(IN PVOID xhciExtension)
{
    DPRINT("XHCI_InterruptNextSOF: function initiated\n");
}

VOID
NTAPI
XHCI_EnableInterrupts(IN PVOID xhciExtension)
{
    DPRINT1("XHCI_EnableInterrupts: function initiated\n");
}

VOID
NTAPI
XHCI_DisableInterrupts(IN PVOID xhciExtension)
{
    DPRINT1("XHCI_DisableInterrupts: function initiated\n");
}

VOID
NTAPI
XHCI_PollController(IN PVOID xhciExtension)
{
    DPRINT("XHCI_PollController: function initiated\n");
}

VOID
NTAPI
XHCI_SetEndpointDataToggle(IN PVOID xhciExtension,
                           IN PVOID xhciEndpoint,
                           IN ULONG DataToggle)
{
    DPRINT1("XHCI_SetEndpointDataToggle: function initiated\n");
}

ULONG
NTAPI
XHCI_GetEndpointStatus(IN PVOID xhciExtension,
                       IN PVOID xhciEndpoint)
{
    DPRINT1("XHCI_GetEndpointStatus: function initiated\n");
    return 0;
}

VOID
NTAPI
XHCI_SetEndpointStatus(IN PVOID xhciExtension,
                       IN PVOID xhciEndpoint,
                       IN ULONG EndpointStatus)
{
    DPRINT1("XHCI_SetEndpointStatus: function initiated\n");
}

MPSTATUS
NTAPI
XHCI_StartSendOnePacket(IN PVOID xhciExtension,
                        IN PVOID PacketParameters,
                        IN PVOID Data,
                        IN PULONG pDataLength,
                        IN PVOID BufferVA,
                        IN PVOID BufferPA,
                        IN ULONG BufferLength,
                        IN USBD_STATUS * pUSBDStatus)
{
    DPRINT1("XHCI_StartSendOnePacket: UNIMPLEMENTED. FIXME\n");
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_EndSendOnePacket(IN PVOID xhciExtension,
                      IN PVOID PacketParameters,
                      IN PVOID Data,
                      IN PULONG pDataLength,
                      IN PVOID BufferVA,
                      IN PVOID BufferPA,
                      IN ULONG BufferLength,
                      IN USBD_STATUS * pUSBDStatus)
{
    DPRINT1("XHCI_EndSendOnePacket: UNIMPLEMENTED. FIXME\n");
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_PassThru(IN PVOID xhciExtension,
              IN PVOID passThruParameters,
              IN ULONG ParameterLength,
              IN PVOID pParameters)
{
    DPRINT1("XHCI_PassThru: UNIMPLEMENTED. FIXME\n");
    return MP_STATUS_SUCCESS;
}

VOID
NTAPI
XHCI_RebalanceEndpoint(IN PVOID ohciExtension,
                       IN PUSBPORT_ENDPOINT_PROPERTIES endpointParameters,
                       IN PVOID ohciEndpoint)
{
    DPRINT1("XHCI_RebalanceEndpoint: UNIMPLEMENTED. FIXME\n");
}

VOID
NTAPI
XHCI_FlushInterrupts(IN PVOID xhciExtension)
{
    DPRINT1("XHCI_FlushInterrupts: function initiated\n");
}

MPSTATUS
NTAPI
XHCI_RH_ChirpRootPort(IN PVOID xhciExtension,
                      IN USHORT Port)
{
    DPRINT1("XHCI_RH_ChirpRootPort: UNIMPLEMENTED. FIXME\n");
    return MP_STATUS_SUCCESS;
}

VOID
NTAPI
XHCI_TakePortControl(IN PVOID ohciExtension)
{
    DPRINT1("XHCI_TakePortControl: UNIMPLEMENTED. FIXME\n");
}

VOID
NTAPI
XHCI_Unload(PDRIVER_OBJECT DriverObject)
{
    DPRINT1("XHCI_Unload: UNIMPLEMENTED. FIXME\n");
}

NTSTATUS
NTAPI
DriverEntry(IN PDRIVER_OBJECT DriverObject,
            IN PUNICODE_STRING RegistryPath)
{
    DPRINT1("DriverEntry: DriverObject - %p, RegistryPath - %wZ\n",
           DriverObject,
           RegistryPath);
    if (USBPORT_GetHciMn() != USBPORT_HCI_MN)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(&RegPacket, sizeof(USBPORT_REGISTRATION_PACKET));

    RegPacket.MiniPortVersion = USB_MINIPORT_VERSION_XHCI;
    /* FIXE: USB_MINIPORT_FLAGS_USB2 on a USB3 driver? */
    RegPacket.MiniPortFlags = USB_MINIPORT_FLAGS_INTERRUPT |
                              USB_MINIPORT_FLAGS_MEMORY_IO |
                              USB_MINIPORT_FLAGS_USB2 |
                              USB_MINIPORT_FLAGS_POLLING |
                              USB_MINIPORT_FLAGS_WAKE_SUPPORT;

    RegPacket.MiniPortBusBandwidth = TOTAL_USB30_BUS_BANDWIDTH;

    RegPacket.MiniPortExtensionSize = 0; // sizeof(XHCI_EXTENSION);
    RegPacket.MiniPortEndpointSize  = 0;  //sizeof(XHCI_ENDPOINT);
    RegPacket.MiniPortTransferSize  = 0;  //sizeof(XHCI_TRANSFER);
    RegPacket.MiniPortResourcesSize = 0; // sizeof(XHCI_HC_RESOURCES);

    RegPacket.OpenEndpoint = XHCI_OpenEndpoint;
    RegPacket.ReopenEndpoint = XHCI_ReopenEndpoint;
    RegPacket.QueryEndpointRequirements = XHCI_QueryEndpointRequirements;
    RegPacket.CloseEndpoint = XHCI_CloseEndpoint;
    RegPacket.StartController = XHCI_StartController;
    RegPacket.StopController = XHCI_StopController;
    RegPacket.SuspendController = XHCI_SuspendController;
    RegPacket.ResumeController = XHCI_ResumeController;
    RegPacket.InterruptService = XHCI_InterruptService;
    RegPacket.InterruptDpc = XHCI_InterruptDpc;
    RegPacket.SubmitTransfer = XHCI_SubmitTransfer;
    RegPacket.SubmitIsoTransfer = XHCI_SubmitIsoTransfer;
    RegPacket.AbortTransfer = XHCI_AbortTransfer;
    RegPacket.GetEndpointState = XHCI_GetEndpointState;
    RegPacket.SetEndpointState = XHCI_SetEndpointState;
    RegPacket.PollEndpoint = XHCI_PollEndpoint;
    RegPacket.CheckController = XHCI_CheckController;
    RegPacket.Get32BitFrameNumber = XHCI_Get32BitFrameNumber;
    RegPacket.InterruptNextSOF = XHCI_InterruptNextSOF;
    RegPacket.EnableInterrupts = XHCI_EnableInterrupts;
    RegPacket.DisableInterrupts = XHCI_DisableInterrupts;
    RegPacket.PollController = XHCI_PollController;
    RegPacket.SetEndpointDataToggle = XHCI_SetEndpointDataToggle;
    RegPacket.GetEndpointStatus = XHCI_GetEndpointStatus;
    RegPacket.SetEndpointStatus = XHCI_SetEndpointStatus;
    RegPacket.RH_GetRootHubData = XHCI_RH_GetRootHubData;
    RegPacket.RH_GetStatus = XHCI_RH_GetStatus;
    RegPacket.RH_GetPortStatus = XHCI_RH_GetPortStatus;
    RegPacket.RH_GetHubStatus = XHCI_RH_GetHubStatus;
    RegPacket.RH_SetFeaturePortReset = XHCI_RH_SetFeaturePortReset;
    RegPacket.RH_SetFeaturePortPower = XHCI_RH_SetFeaturePortPower;
    RegPacket.RH_SetFeaturePortEnable = XHCI_RH_SetFeaturePortEnable;
    RegPacket.RH_SetFeaturePortSuspend = XHCI_RH_SetFeaturePortSuspend;
    RegPacket.RH_ClearFeaturePortEnable = XHCI_RH_ClearFeaturePortEnable;
    RegPacket.RH_ClearFeaturePortPower = XHCI_RH_ClearFeaturePortPower;
    RegPacket.RH_ClearFeaturePortSuspend = XHCI_RH_ClearFeaturePortSuspend;
    RegPacket.RH_ClearFeaturePortEnableChange = XHCI_RH_ClearFeaturePortEnableChange;
    RegPacket.RH_ClearFeaturePortConnectChange = XHCI_RH_ClearFeaturePortConnectChange;
    RegPacket.RH_ClearFeaturePortResetChange = XHCI_RH_ClearFeaturePortResetChange;
    RegPacket.RH_ClearFeaturePortSuspendChange = XHCI_RH_ClearFeaturePortSuspendChange;
    RegPacket.RH_ClearFeaturePortOvercurrentChange = XHCI_RH_ClearFeaturePortOvercurrentChange;
    RegPacket.RH_DisableIrq = XHCI_RH_DisableIrq;
    RegPacket.RH_EnableIrq = XHCI_RH_EnableIrq;
    RegPacket.StartSendOnePacket = XHCI_StartSendOnePacket;
    RegPacket.EndSendOnePacket = XHCI_EndSendOnePacket;
    RegPacket.PassThru = XHCI_PassThru;
    RegPacket.RebalanceEndpoint = XHCI_RebalanceEndpoint;
    RegPacket.FlushInterrupts = XHCI_FlushInterrupts;
    RegPacket.RH_ChirpRootPort = XHCI_RH_ChirpRootPort;
    RegPacket.TakePortControl = XHCI_TakePortControl;

    DPRINT1("XHCI_DriverEntry: before driver unload. FIXME\n");
    DriverObject->DriverUnload = XHCI_Unload;

    DPRINT1("XHCI_DriverEntry: after driver unload, before usbport_reg call. FIXME\n");

    return USBPORT_RegisterUSBPortDriver(DriverObject, USB30_MINIPORT_INTERFACE_VERSION, &RegPacket);
}
