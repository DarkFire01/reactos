/*
 * PROJECT:     FreeLoader UEFI Support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Keeps the votes of Qualcomm firmware the USB kernel debugger needs past boot services
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <uefildr.h>

#include <debug.h>
DBG_DEFAULT_CHANNEL(WARNING);

/*
 * When boot services exit, the firmware's USB driver puts the primary controller
 * into low power: it drops the controller's bus bandwidth to nothing and moves
 * the PHY supplies to their low power level. The debugger brings the controller
 * back without the bus or the supplies, so the bus stalls once it carries DMA.
 * Votes this loader places through the firmware's NPA service are never
 * released, so they hold after the firmware's own are gone.
 */

/* Byte offsets of the calls in the firmware's NPA protocol */
#define NPA_CREATE_CLIENT               0x10
#define NPA_REQUIRED_REQUEST            0x28
#define NPA_VECTOR_REQUEST              0x90

#define NPA_CLIENT_REQUIRED             0x40
#define NPA_CLIENT_VECTOR               0x400

typedef
EFI_STATUS
(EFIAPI *PNPA_CREATE_CLIENT)(
    _In_ PCSTR ResourceName,
    _In_ PCSTR ClientName,
    _In_ UINT32 Type,
    _In_ UINT32 Size,
    _In_opt_ PVOID Data,
    _Out_ PVOID* Client);

typedef
VOID
(EFIAPI *PNPA_REQUIRED_REQUEST)(
    _In_ PVOID Client,
    _In_ UINT32 State);

typedef
VOID
(EFIAPI *PNPA_VECTOR_REQUEST)(
    _In_ PVOID Client,
    _In_ UINT32 Count,
    _In_ PVOID Vector);

static EFI_GUID NpaProtocolGuid =
    { 0x79D6C870, 0x725E, 0x489E, { 0xA0, 0xA1, 0x27, 0xE7, 0xA5, 0xD0, 0xCB, 0x35 } };

/* Bus routes of the primary controller: the processor to its registers and the controller to memory */
static UINT32 UsbBusRoutes[] = { 0x00, 0x16, 0x20, 0x00 };

/* The bandwidth the firmware asks for while the controller runs, with the count it passes */
static UINT64 UsbBusBandwidth[] = { 4, 74000000, 0, 0 };
#define USB_BUS_BANDWIDTH_COUNT         16

/* The PHY supply level the firmware asks for while the controller runs */
#define USB_SUPPLY_ACTIVE               2

extern EFI_SYSTEM_TABLE* GlobalSystemTable;

static
PVOID
UefiNpaCall(
    _In_ PUCHAR Npa,
    _In_ ULONG Offset)
{
    return *(PVOID*)(Npa + Offset);
}

/**
 * @brief Votes the bus and supplies of the primary USB controller through the firmware, for good.
 */
VOID
UefiKeepUsbVotes(VOID)
{
    PNPA_CREATE_CLIENT CreateClient;
    PNPA_REQUIRED_REQUEST RequiredRequest;
    PNPA_VECTOR_REQUEST VectorRequest;
    PVOID Supply = NULL;
    PVOID Bus = NULL;
    PUCHAR Npa = NULL;
    EFI_STATUS Status;

    Status = GlobalSystemTable->BootServices->LocateProtocol(&NpaProtocolGuid, NULL, (VOID**)&Npa);
    if (Status != EFI_SUCCESS || Npa == NULL)
    {
        TuiPrintf("freeldr: no NPA service for the USB votes: %lx\n", (ULONG)Status);
        return;
    }

    CreateClient = (PNPA_CREATE_CLIENT)UefiNpaCall(Npa, NPA_CREATE_CLIENT);
    RequiredRequest = (PNPA_REQUIRED_REQUEST)UefiNpaCall(Npa, NPA_REQUIRED_REQUEST);
    VectorRequest = (PNPA_VECTOR_REQUEST)UefiNpaCall(Npa, NPA_VECTOR_REQUEST);

    /* Phones with a PMI8998 name the supplies after the first port, others after the third */
    CreateClient("/pmic/client/usb_ss1", "freeldr_usb_ss1", NPA_CLIENT_REQUIRED, 0, NULL, &Supply);
    if (Supply == NULL)
        CreateClient("/pmic/client/usb_ss3", "freeldr_usb_ss3", NPA_CLIENT_REQUIRED, 0, NULL, &Supply);
    if (Supply != NULL)
        RequiredRequest(Supply, USB_SUPPLY_ACTIVE);

    CreateClient("/icb/arbiter",
                 "freeldr_usb_bus",
                 NPA_CLIENT_VECTOR,
                 sizeof(UsbBusRoutes),
                 UsbBusRoutes,
                 &Bus);
    if (Bus != NULL)
        VectorRequest(Bus, USB_BUS_BANDWIDTH_COUNT, UsbBusBandwidth);

    TuiPrintf("freeldr: USB votes kept, supplies %s, bus %s\n",
              Supply ? "on" : "not voted",
              Bus ? "on" : "not voted");
}
