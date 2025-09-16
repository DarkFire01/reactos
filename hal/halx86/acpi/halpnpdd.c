/*
 * PROJECT:         ReactOS HAL
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            hal/halx86/acpi/halpnpdd.c
 * PURPOSE:         HAL Plug and Play Device Driver
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <hal.h>
#include "dispatch.h"
extern FADT HalpFixedAcpiDescTable;
#include <initguid.h>
#include <wdmguid.h>
#include <acpi.h>
/* ACPI_BIOS_ERROR defined in acoutput.h and bugcodes.h */
#undef ACPI_BIOS_ERROR
#include <smp.h>
extern HALP_APIC_INFO_TABLE HalpApicInfoTable;
#include <debug.h>
static
UCHAR
HalpComputePciLegacyLine(
    IN ULONG Bus,
    IN ULONG SlotNumber,
    IN UCHAR InterruptPin)
{
    UCHAR pinIndex = (InterruptPin ? (InterruptPin - 1) : 0) & 0x3;
    
    DPRINT1("[ACPI] HalpComputePciLegacyLine: Bus=%lu Slot=%lu Pin=%u\n", Bus, SlotNumber, InterruptPin);

    /* If we are in PIC mode, return PIC IRQs using the classic 9/10/11/5 swizzle */
    if (HalpAcpiPicStateIntact())
    {
        static const UCHAR PicRoute[4] = { 9, 10, 11, 5 };
        UCHAR irq = PicRoute[((SlotNumber & 0x1F) + pinIndex) & 0x3];
        DPRINT("[ACPI] Legacy PIC route: bus %lu slot %lu pin %u -> IRQ %u\n",
                Bus, SlotNumber & 0x1F, InterruptPin, irq);
        return irq;
    }

    /* APIC/IOAPIC: return a GSI in 16.. range with bus-based swizzle */
    if (Bus == 0)
    {
        UCHAR gsi = (UCHAR)(16 + (((SlotNumber & 0x1F) + pinIndex) & 0x3));
        DPRINT("[ACPI] Legacy APIC route: bus %lu slot %lu pin %u -> GSI %u\n",
                Bus, SlotNumber & 0x1F, InterruptPin, gsi);
        return gsi;
    }
    else
    {
        UCHAR gsi = (UCHAR)(16 + (((Bus & 0x07) * 4) + (((SlotNumber & 0x1F) + pinIndex) & 0x3)));
        DPRINT("[ACPI] Legacy APIC route: bus %lu slot %lu pin %u -> GSI %u\n",
                Bus, SlotNumber & 0x1F, InterruptPin, gsi);
        return gsi;
    }
}

/* Simple HAL-managed PCI link nodes (LNKA..LNKD) for ACPI _PRT fallback */
typedef struct _HALP_LINK_NODE
{
    UCHAR AssignedGsi;
    UCHAR Initialized;
} HALP_LINK_NODE, *PHALP_LINK_NODE;

static HALP_LINK_NODE HalpLinkNodes[4]; /* A..D */
static volatile LONG HalpLinkNodesInit;

static VOID
HalpInitLinkNodesOnce(VOID)
{
    if (InterlockedCompareExchange(&HalpLinkNodesInit, 1, 0) == 0)
    {
        for (int i = 0; i < 4; ++i)
        {
            HalpLinkNodes[i].AssignedGsi = 0xFF;
            HalpLinkNodes[i].Initialized = 1;
        }
    }
}

static UCHAR
HalpChooseGsiForLink(UCHAR linkIndex)
{
    /* Prefer typical PCI INTx shareable GSIs: 9,10,11,5, then 3..15 excluding 0,1,2,8,13 */
    static const UCHAR PrefList[] = {9, 10, 11, 5, 3, 4, 6, 7, 12, 14, 15};
    for (size_t i = 0; i < sizeof(PrefList); ++i)
    {
        UCHAR g = PrefList[i];
        /* Honour MADT ISA overrides mapping IRQ->GSI if present */
        if (g < 16)
        {
            ULONG over = HalpApicInfoTable.IsaOverrideGsi[g];
            if (over != 0) return (UCHAR)over;
        }
        return g;
    }
    return 9;
}

/*
 * _PRT/Link routing fallback in HAL: map (bus, device, pin) to one of LNKA..LNKD
 * and assign a stable GSI, programming will occur on enable.
 */
static BOOLEAN
HalpResolvePrtGsi(
    IN ULONG Bus,
    IN ULONG DeviceNumber,
    IN UCHAR InterruptPin,
    OUT PULONG Gsi,
    OUT PUCHAR Trigger,
    OUT PUCHAR Polarity,
    OUT PVOID *LinkNode)
{
    UCHAR pinIndex;
    UCHAR linkIndex;

    DPRINT1("[ACPI] HalpResolvePrtGsi: Bus=%lu Device=%lu Pin=%u\n", Bus, DeviceNumber, InterruptPin);

    if (LinkNode) *LinkNode = NULL;

    /* No INTx pin present */
    if (InterruptPin == 0)
    {
        *Gsi = 0;
        *Trigger = 3;
        *Polarity = 3;
        return FALSE;
    }

    /* Default PCI implies level/low */
    *Trigger = 3;  /* ACPI_LEVEL_SENSITIVE */
    *Polarity = 3; /* ACPI_ACTIVE_LOW */

    HalpInitLinkNodesOnce();

    /* Barber-pole swizzle reduced to link selection at this bus level */
    pinIndex = (InterruptPin - 1) & 0x3;
    linkIndex = (UCHAR)(((DeviceNumber & 0x1F) + pinIndex) & 0x3); /* A..D */

    if (HalpLinkNodes[linkIndex].AssignedGsi == 0xFF)
    {
        UCHAR chosen = HalpChooseGsiForLink(linkIndex);
        HalpLinkNodes[linkIndex].AssignedGsi = chosen;
        DPRINT("[ACPI] Link LNK%c assigned GSI %u (bus %lu dev %lu pin %u)\n",
                'A' + linkIndex, chosen, Bus, DeviceNumber, InterruptPin);
    }

    *Gsi = HalpLinkNodes[linkIndex].AssignedGsi;
    return TRUE;
}

#define NDEBUG
#include <debug.h>

/* ACPI Register Interface Definitions */
typedef enum _ACPI_REG_TYPE {
    PM1a_ENABLE,
    PM1b_ENABLE,
    PM1a_STATUS,
    PM1b_STATUS,
    PM1a_CONTROL,
    PM1b_CONTROL,
    GP_STATUS,
    GP_ENABLE,
    SMI_CMD,
    MaxRegType
} ACPI_REG_TYPE, *PACPI_REG_TYPE;

typedef USHORT (NTAPI *PREAD_ACPI_REGISTER) (
  ACPI_REG_TYPE AcpiReg,
  ULONG         Register);

typedef VOID (NTAPI *PWRITE_ACPI_REGISTER) (
  ACPI_REG_TYPE AcpiReg,
  ULONG         Register,
  USHORT        Value
  );

typedef struct ACPI_REGS_INTERFACE_STANDARD {
    //
    // generic interface header
    //
    USHORT Size;
    USHORT Version;
    PVOID  Context;
    PINTERFACE_REFERENCE   InterfaceReference;
    PINTERFACE_DEREFERENCE InterfaceDereference;

    //
    // READ/WRITE_ACPI_REGISTER functions
    //
    PREAD_ACPI_REGISTER  ReadAcpiRegister;
    PWRITE_ACPI_REGISTER WriteAcpiRegister;

} ACPI_REGS_INTERFACE_STANDARD, *PACPI_REGS_INTERFACE_STANDARD;

typedef NTSTATUS (NTAPI *PHAL_QUERY_ALLOCATE_PORT_RANGE) (
  BOOLEAN IsSparse,
  BOOLEAN PrimaryIsMmio,
  PVOID VirtBaseAddr,
  PHYSICAL_ADDRESS PhysBaseAddr,
  ULONG Length,
  PUSHORT NewRangeId
  );

typedef VOID (NTAPI *PHAL_FREE_PORT_RANGE)(
    USHORT RangeId
    );

typedef struct _HAL_PORT_RANGE_INTERFACE {
    //
    // generic interface header
    //
    USHORT Size;
    USHORT Version;
    PVOID  Context;
    PINTERFACE_REFERENCE   InterfaceReference;
    PINTERFACE_DEREFERENCE InterfaceDereference;

    //
    // QueryAllocateRange/FreeRange functions
    //
    PHAL_QUERY_ALLOCATE_PORT_RANGE QueryAllocateRange;
    PHAL_FREE_PORT_RANGE FreeRange;

} HAL_PORT_RANGE_INTERFACE, *PHAL_PORT_RANGE_INTERFACE;

#define HAL_PORT_RANGE_INTERFACE_VERSION 1


ULONG
NTAPI
HalpSetBusData(
    PVOID Context,
    ULONG DataType,
    PVOID Buffer,
    ULONG Offset,
    ULONG Length)
{
    /* For now, return 0 to indicate no data set */
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(DataType);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Offset);
    
    DPRINT("HalpSetBusData: DataType=%d, Offset=%d, Length=%d (stub)\n",
           DataType, Offset, Length);
    
    return 0;
}

ULONG
NTAPI
HalpGetBusData(
    PVOID Context,
    ULONG DataType,
    PVOID Buffer,
    ULONG Offset,
    ULONG Length)
{
    /* For now, return 0 to indicate no data retrieved */
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(DataType);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Offset);
    
    DPRINT("HalpGetBusData: DataType=%d, Offset=%d, Length=%d (stub)\n",
           DataType, Offset, Length);
    
    return 0;
}

typedef enum _EXTENSION_TYPE
{
    PdoExtensionType = 0xC0,
    FdoExtensionType
} EXTENSION_TYPE;

typedef enum _PDO_TYPE
{
    AcpiPdo = 0x80,
    WdPdo
} PDO_TYPE;

typedef struct _FDO_EXTENSION
{
    EXTENSION_TYPE ExtensionType;
    struct _PDO_EXTENSION* ChildPdoList;
    PDEVICE_OBJECT PhysicalDeviceObject;
    PDEVICE_OBJECT FunctionalDeviceObject;
    PDEVICE_OBJECT AttachedDeviceObject;
} FDO_EXTENSION, *PFDO_EXTENSION;

typedef struct _PDO_EXTENSION
{
    EXTENSION_TYPE ExtensionType;
    struct _PDO_EXTENSION* Next;
    PDEVICE_OBJECT PhysicalDeviceObject;
    PFDO_EXTENSION ParentFdoExtension;
    PDO_TYPE PdoType;
    PDESCRIPTION_HEADER WdTable;
    LONG InterfaceReferenceCount;
} PDO_EXTENSION, *PPDO_EXTENSION;

/* GLOBALS ********************************************************************/

PDRIVER_OBJECT HalpDriverObject;

/* PRIVATE FUNCTIONS **********************************************************/

NTSTATUS
NTAPI
HalpAddDevice(IN PDRIVER_OBJECT DriverObject,
              IN PDEVICE_OBJECT TargetDevice)
{
    NTSTATUS Status;
    PFDO_EXTENSION FdoExtension;
    PPDO_EXTENSION PdoExtension;
    PDEVICE_OBJECT DeviceObject, AttachedDevice;
    PDEVICE_OBJECT PdoDeviceObject;
    PDESCRIPTION_HEADER Wdrt;

    DPRINT("HAL: PnP Driver ADD!\n");

    /* Create the FDO */
    Status = IoCreateDevice(DriverObject,
                            sizeof(FDO_EXTENSION),
                            NULL,
                            FILE_DEVICE_BUS_EXTENDER,
                            0,
                            FALSE,
                            &DeviceObject);
    if (!NT_SUCCESS(Status))
    {
        /* Should not happen */
        DbgBreakPoint();
        return Status;
    }

    /* Setup the FDO extension */
    FdoExtension = DeviceObject->DeviceExtension;
    FdoExtension->ExtensionType = FdoExtensionType;
    FdoExtension->PhysicalDeviceObject = TargetDevice;
    FdoExtension->FunctionalDeviceObject = DeviceObject;
    FdoExtension->ChildPdoList = NULL;

    /* FDO is done initializing */
    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    /* Attach to the physical device object (the bus) */
    AttachedDevice = IoAttachDeviceToDeviceStack(DeviceObject, TargetDevice);
    if (!AttachedDevice)
    {
        /* Failed, undo everything */
        IoDeleteDevice(DeviceObject);
        return STATUS_NO_SUCH_DEVICE;
    }

    /* Save the attachment */
    FdoExtension->AttachedDeviceObject = AttachedDevice;

    /* Create the PDO */
    Status = IoCreateDevice(DriverObject,
                            sizeof(PDO_EXTENSION),
                            NULL,
                            FILE_DEVICE_BUS_EXTENDER,
                            FILE_AUTOGENERATED_DEVICE_NAME,
                            FALSE,
                            &PdoDeviceObject);
    if (!NT_SUCCESS(Status))
    {
        /* Fail */
        DPRINT1("HAL: Could not create ACPI device object status=0x%08x\n", Status);
        return Status;
    }

    /* Setup the PDO device extension */
    PdoExtension = PdoDeviceObject->DeviceExtension;
    PdoExtension->ExtensionType = PdoExtensionType;
    PdoExtension->PhysicalDeviceObject = PdoDeviceObject;
    PdoExtension->ParentFdoExtension = FdoExtension;
    PdoExtension->PdoType = AcpiPdo;

    /* Add the PDO to the head of the list */
    PdoExtension->Next = FdoExtension->ChildPdoList;
    FdoExtension->ChildPdoList = PdoExtension;

    /* Initialization is finished */
    PdoDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    /* Find the ACPI watchdog table */
    Wdrt = HalAcpiGetTable(0, 'TRDW');
    if (Wdrt)
    {
        /* FIXME: TODO */
        DPRINT1("You have an ACPI Watchdog. That's great! You should be proud ;-)\n");
    }

    /* Return status */
    DPRINT("Device added %lx\n", Status);
    return Status;
}

/* ACPI Register Interface Implementation */
USHORT
NTAPI
HalpReadAcpiRegister(
    ACPI_REG_TYPE AcpiReg,
    ULONG Register)
{
    USHORT value = 0;
    ULONG port = 0;
    PFADT Fadt = &HalpFixedAcpiDescTable;

    UNREFERENCED_PARAMETER(Register);

    switch (AcpiReg)
    {
        case PM1a_STATUS:
            port = Fadt->pm1a_evt_blk_io_port;
            if (Fadt->pm1_evt_len >= 2 && port)
                value = READ_PORT_USHORT((PUSHORT)(ULONG_PTR)port);
            break;

        case PM1b_STATUS:
            port = Fadt->pm1b_evt_blk_io_port;
            if (Fadt->pm1_evt_len >= 2 && port)
                value = READ_PORT_USHORT((PUSHORT)(ULONG_PTR)port);
            break;

        case PM1a_ENABLE:
            port = Fadt->pm1a_evt_blk_io_port;
            if (Fadt->pm1_evt_len >= 4 && port)
                value = READ_PORT_USHORT((PUSHORT)(ULONG_PTR)(port + 2));
            break;

        case PM1b_ENABLE:
            port = Fadt->pm1b_evt_blk_io_port;
            if (Fadt->pm1_evt_len >= 4 && port)
                value = READ_PORT_USHORT((PUSHORT)(ULONG_PTR)(port + 2));
            break;

        case PM1a_CONTROL:
            port = Fadt->pm1a_ctrl_blk_io_port;
            if (Fadt->pm1_ctrl_len >= 2 && port)
                value = READ_PORT_USHORT((PUSHORT)(ULONG_PTR)port);
            break;

        case PM1b_CONTROL:
            port = Fadt->pm1b_ctrl_blk_io_port;
            if (Fadt->pm1_ctrl_len >= 2 && port)
                value = READ_PORT_USHORT((PUSHORT)(ULONG_PTR)port);
            break;

        case GP_STATUS:
            port = Fadt->gp0_blk_io_port;
            if (Fadt->gp0_blk_len && port)
                value = READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)(port + Register));
            break;

        case GP_ENABLE:
            port = Fadt->gp0_blk_io_port;
            if (Fadt->gp0_blk_len && port)
            {
                ULONG enableOffset = Fadt->gp0_blk_len / 2; /* status then enable */
                value = READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)(port + enableOffset + Register));
            }
            break;

        case SMI_CMD:
            port = Fadt->smi_cmd_io_port;
            if (port)
                value = READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)port);
            break;

        default:
            DPRINT1("Unknown ACPI register type: %d\n", AcpiReg);
            break;
    }

    return value;
}

VOID
NTAPI
HalpWriteAcpiRegister(
    ACPI_REG_TYPE AcpiReg,
    ULONG Register,
    USHORT Value)
{
    ULONG port = 0;
    PFADT Fadt = (PFADT)&HalpFixedAcpiDescTable;

    UNREFERENCED_PARAMETER(Register);

    switch (AcpiReg)
    {
        case PM1a_STATUS:
            port = Fadt->pm1a_evt_blk_io_port;
            if (Fadt->pm1_evt_len >= 2 && port)
                WRITE_PORT_USHORT((PUSHORT)(ULONG_PTR)port, Value);
            break;

        case PM1b_STATUS:
            port = Fadt->pm1b_evt_blk_io_port;
            if (Fadt->pm1_evt_len >= 2 && port)
                WRITE_PORT_USHORT((PUSHORT)(ULONG_PTR)port, Value);
            break;

        case PM1a_ENABLE:
            port = Fadt->pm1a_evt_blk_io_port;
            if (Fadt->pm1_evt_len >= 4 && port)
                WRITE_PORT_USHORT((PUSHORT)(ULONG_PTR)(port + 2), Value);
            break;

        case PM1b_ENABLE:
            port = Fadt->pm1b_evt_blk_io_port;
            if (Fadt->pm1_evt_len >= 4 && port)
                WRITE_PORT_USHORT((PUSHORT)(ULONG_PTR)(port + 2), Value);
            break;

        case PM1a_CONTROL:
            port = Fadt->pm1a_ctrl_blk_io_port;
            if (Fadt->pm1_ctrl_len >= 2 && port)
                WRITE_PORT_USHORT((PUSHORT)(ULONG_PTR)port, Value);
            break;

        case PM1b_CONTROL:
            port = Fadt->pm1b_ctrl_blk_io_port;
            if (Fadt->pm1_ctrl_len >= 2 && port)
                WRITE_PORT_USHORT((PUSHORT)(ULONG_PTR)port, Value);
            break;

        case GP_STATUS:
        {
            port = Fadt->gp0_blk_io_port;
            if (Fadt->gp0_blk_len && port)
            {
                /* GPx_STS are write-1-to-clear */
                WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)(port + Register), (UCHAR)Value);
            }
            break;
        }

        case GP_ENABLE:
        {
            port = Fadt->gp0_blk_io_port;
            if (Fadt->gp0_blk_len && port)
            {
                ULONG enableOffset = Fadt->gp0_blk_len / 2;
                WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)(port + enableOffset + Register), (UCHAR)Value);
            }
            break;
        }

        case SMI_CMD:
            port = Fadt->smi_cmd_io_port;
            if (port)
                WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)port, (UCHAR)Value);
            break;

        default:
            DPRINT1("Unknown ACPI register type: %d\n", AcpiReg);
            break;
    }
}

/* Port Range Interface Implementation */
NTSTATUS
NTAPI
HalpQueryAllocatePortRange(
    BOOLEAN IsSparse,
    BOOLEAN PrimaryIsMmio,
    PVOID VirtBaseAddr,
    PHYSICAL_ADDRESS PhysBaseAddr,
    ULONG Length,
    PUSHORT NewRangeId)
{
    /* For now, provide a stub implementation */
    DPRINT("HalpQueryAllocatePortRange: IsSparse=%d, PrimaryIsMmio=%d, PhysAddr=0x%I64x, Length=0x%x\n",
           IsSparse, PrimaryIsMmio, PhysBaseAddr.QuadPart, Length);
    
    /* TODO: Implement actual port range allocation */
    /* For now, just assign a dummy range ID */
    if (NewRangeId)
    {
        *NewRangeId = 1; /* Dummy range ID */
    }
    
    return STATUS_SUCCESS;
}

VOID
NTAPI
HalpFreePortRange(
    USHORT RangeId)
{
    /* For now, provide a stub implementation */
    DPRINT("HalpFreePortRange: RangeId=%d\n", RangeId);
    
    /* TODO: Implement actual port range deallocation */
}

/* Interface reference/dereference functions */
VOID
NTAPI
HalPnpInterfaceReference(
    PVOID Context)
{
    /* Reference the interface */
    UNREFERENCED_PARAMETER(Context);
}

VOID
NTAPI
HalPnpInterfaceDereference(
    PVOID Context)
{
    /* Dereference the interface */
    UNREFERENCED_PARAMETER(Context);
}

NTSTATUS
NTAPI
HalpGetInterruptRouting(
    IN PDEVICE_OBJECT Pdo,
    OUT ULONG *Bus,
    OUT ULONG *PciSlot,
    OUT UCHAR *InterruptLine,
    OUT UCHAR *InterruptPin,
    OUT UCHAR *ClassCode,
    OUT UCHAR *SubClassCode,
    OUT PDEVICE_OBJECT *ParentPdo,
    OUT ROUTING_TOKEN *RoutingToken,
    OUT UCHAR *Flags)
{
    PPCI_COMMON_CONFIG PciConfig;
    BUS_HANDLER BusHandler;
    ULONG Bytes;
    PCI_SLOT_NUMBER Slot;
    ULONG Address;

    DPRINT1("[ACPI] HalpGetInterruptRouting called for PDO %p\n", Pdo);

    if (!Pdo || !Bus || !PciSlot || !InterruptLine || !InterruptPin ||
        !ClassCode || !SubClassCode || !ParentPdo || !RoutingToken || !Flags)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Default outputs */
    *ParentPdo = NULL;
    RtlZeroMemory(RoutingToken, sizeof(*RoutingToken));
    *Flags = 0;

    /* Query PDO bus number */
    if (!NT_SUCCESS(IoGetDeviceProperty(Pdo,
                                       DevicePropertyBusNumber,
                                       sizeof(ULONG),
                                       Bus,
                                       &Bytes)))
    {
        return STATUS_UNSUCCESSFUL;
    }

    /* Read PCI config to get slot, class, pin/line */
    /* Prefer querying the slot from PDO's DevicePropertyAddress */
    if (!NT_SUCCESS(IoGetDeviceProperty(Pdo,
                                       DevicePropertyAddress,
                                       sizeof(ULONG),
                                       &Address,
                                       &Bytes)))
    {
        Address = 0; /* fallback */
    }
    Slot.u.AsULONG = Address;
    PciConfig = ExAllocatePoolWithTag(NonPagedPool, sizeof(PCI_COMMON_CONFIG), 'ICPH');
    if (!PciConfig) return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(&BusHandler, sizeof(BUS_HANDLER));
    BusHandler.InterfaceType = PCIBus;
    BusHandler.BusNumber = *Bus;

    Bytes = HalGetBusDataByOffset(PCIConfiguration,
                                  *Bus,
                                  Slot.u.AsULONG,
                                  PciConfig,
                                  0,
                                  sizeof(PCI_COMMON_CONFIG));
    if (Bytes < FIELD_OFFSET(PCI_COMMON_CONFIG, BaseClass))
    {
        ExFreePoolWithTag(PciConfig, 'ICPH');
        return STATUS_UNSUCCESSFUL;
    }

    *PciSlot = (ULONG)Slot.u.AsULONG;
    *ClassCode = PciConfig->BaseClass;
    *SubClassCode = PciConfig->SubClass;
    *InterruptPin = PciConfig->u.type0.InterruptPin;
    *InterruptLine = PciConfig->u.type0.InterruptLine;

    /* If no line assigned, derive a legacy line via swizzle as fallback */
    if ((*InterruptLine == 0) || (*InterruptLine == 0xFF))
    {
        *InterruptLine = HalpComputePciLegacyLine(*Bus, *PciSlot, *InterruptPin);
    }

    /* Resolve GSI via ACPI _PRT if possible */
    {
        ULONG Gsi;
        UCHAR Trig, Pol;
        PVOID Link;
        if (HalpResolvePrtGsi(*Bus, (*PciSlot) & 0x1F, *InterruptPin, &Gsi, &Trig, &Pol, &Link))
        {
            KIRQL Irql;
            KAFFINITY Affinity;
            ULONG Vector = HalGetInterruptVector(PCIBus,
                                                 *Bus,
                                                 Gsi,
                                                 Gsi,
                                                 &Irql,
                                                 &Affinity);
            RoutingToken->StaticVector = Vector;
            RoutingToken->LinkNode = Link;
            RoutingToken->Flags = 0;
            /* Backfill InterruptLine with legacy value for drivers */
            if (*InterruptLine == 0 || *InterruptLine == 0xFF)
            {
                *InterruptLine = HalpComputePciLegacyLine(*Bus, *PciSlot, *InterruptPin);
            }
        }
        else
        {
            /* Fallback to legacy vector computation */
            KIRQL Irql;
            KAFFINITY Affinity;
            ULONG Vector = HalGetInterruptVector(PCIBus,
                                                 *Bus,
                                                 *InterruptLine,
                                                 *InterruptLine,
                                                 &Irql,
                                                 &Affinity);
            RoutingToken->StaticVector = Vector;
            RoutingToken->LinkNode = NULL;
            RoutingToken->Flags = 0;
        }
    }

    ExFreePoolWithTag(PciConfig, 'ICPH');
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
HalpSetInterruptRoutingToken(
    IN PDEVICE_OBJECT Pdo,
    IN PROUTING_TOKEN RoutingToken)
{
    UNREFERENCED_PARAMETER(Pdo);
    UNREFERENCED_PARAMETER(RoutingToken);
    /* TODO: Store routing token for device */
    return STATUS_SUCCESS;
}

VOID
NTAPI
HalpUpdateInterruptLine(
    IN PDEVICE_OBJECT Pdo,
    IN UCHAR LineRegister)
{
    ULONG Bus;
    ULONG Bytes;
    PCI_SLOT_NUMBER Slot;
    ULONG Address;

    DPRINT1("[ACPI] HalpUpdateInterruptLine called: PDO %p, LineRegister %u\n", Pdo, LineRegister);

    if (!Pdo) return;

    if (!NT_SUCCESS(IoGetDeviceProperty(Pdo,
                                       DevicePropertyBusNumber,
                                       sizeof(ULONG),
                                       &Bus,
                                       &Bytes)))
    {
        return;
    }

    if (!NT_SUCCESS(IoGetDeviceProperty(Pdo,
                                       DevicePropertyAddress,
                                       sizeof(ULONG),
                                       &Address,
                                       &Bytes)))
    {
        Address = 0;
    }
    Slot.u.AsULONG = Address;

    /* Program IOAPIC redirection and enable interrupt for this device */
    {
        ULONG Gsi;
        UCHAR Trig, Pol;
        PVOID Link;
        KIRQL Irql;
        KAFFINITY Affinity;
        ULONG Address = 0;
        UCHAR Pin;
        PCI_COMMON_CONFIG Cfg;

        IoGetDeviceProperty(Pdo, DevicePropertyAddress, sizeof(ULONG), &Address, &Bytes);
        /* Read pin from PCI config */
        HalGetBusDataByOffset(PCIConfiguration,
                               Bus,
                               Address,
                               &Cfg,
                               0,
                               FIELD_OFFSET(PCI_COMMON_CONFIG, u.type0.InterruptPin) + sizeof(Cfg.u.type0.InterruptPin));
        Pin = Cfg.u.type0.InterruptPin;

        if (HalpResolvePrtGsi(Bus, Address & 0x1F, Pin, &Gsi, &Trig, &Pol, &Link) && Gsi)
        {
            ULONG Vector = HalGetInterruptVector(PCIBus, Bus, Gsi, Gsi, &Irql, &Affinity);
            if (Vector)
            {
                HalEnableSystemInterrupt(Vector, Irql, (Trig == 3) ? LevelSensitive : Latched);
            }
        }
        else if (LineRegister != 0 && LineRegister != 0xFF)
        {
            ULONG Vector = HalGetInterruptVector(PCIBus,
                                                 Bus,
                                                 LineRegister,
                                                 LineRegister,
                                                 &Irql,
                                                 &Affinity);
            if (Vector)
            {
                HalEnableSystemInterrupt(Vector, Irql, LevelSensitive);
            }
        }
    }

    /* Update the device's PCI InterruptLine register */
    HalSetBusDataByOffset(PCIConfiguration,
                          Bus,
                          Slot.u.AsULONG,
                          &LineRegister,
                          FIELD_OFFSET(PCI_COMMON_CONFIG, u.type0.InterruptLine),
                          sizeof(LineRegister));
}

NTSTATUS
NTAPI
HalpQueryInterface(IN PDEVICE_OBJECT DeviceObject,
                   IN CONST GUID* InterfaceType,
                   IN USHORT Version,
                   IN PVOID InterfaceSpecificData,
                   IN ULONG InterfaceBufferSize,
                   IN PINTERFACE Interface,
                   OUT PULONG Length)
{
    if (IsEqualIID(InterfaceType, &GUID_ACPI_REGS_INTERFACE_STANDARD))
    {
        PACPI_REGS_INTERFACE_STANDARD AcpiRegInterface;
        
        DPRINT("HalpQueryInterface(GUID_ACPI_REGS_INTERFACE_STANDARD)\n");
        
        /* Set the interface size */
        *Length = sizeof(ACPI_REGS_INTERFACE_STANDARD);
        
        /* Check buffer size */
        if (InterfaceBufferSize < sizeof(ACPI_REGS_INTERFACE_STANDARD))
        {
            return STATUS_BUFFER_TOO_SMALL;
        }
        
        /* Fill in the interface */
        AcpiRegInterface = (PACPI_REGS_INTERFACE_STANDARD)Interface;
        AcpiRegInterface->Size = sizeof(ACPI_REGS_INTERFACE_STANDARD);
        AcpiRegInterface->Version = Version;
        AcpiRegInterface->Context = DeviceObject;
        AcpiRegInterface->InterfaceReference = HalPnpInterfaceReference;
        AcpiRegInterface->InterfaceDereference = HalPnpInterfaceDereference;
        AcpiRegInterface->ReadAcpiRegister = HalpReadAcpiRegister;
        AcpiRegInterface->WriteAcpiRegister = HalpWriteAcpiRegister;
        
        return STATUS_SUCCESS;
    }
    else if (IsEqualIID(InterfaceType, &GUID_ACPI_PORT_RANGES_INTERFACE_STANDARD))
    {
        PHAL_PORT_RANGE_INTERFACE PortRangeInterface;
        
        DPRINT("HalpQueryInterface(GUID_ACPI_PORT_RANGES_INTERFACE_STANDARD)\n");
        
        /* Set the interface size */
        *Length = sizeof(HAL_PORT_RANGE_INTERFACE);
        
        /* Check buffer size */
        if (InterfaceBufferSize < sizeof(HAL_PORT_RANGE_INTERFACE))
        {
            return STATUS_BUFFER_TOO_SMALL;
        }
        
        /* Fill in the interface */
        PortRangeInterface = (PHAL_PORT_RANGE_INTERFACE)Interface;
        PortRangeInterface->Size = sizeof(HAL_PORT_RANGE_INTERFACE);
        PortRangeInterface->Version = HAL_PORT_RANGE_INTERFACE_VERSION;
        PortRangeInterface->Context = DeviceObject;
        PortRangeInterface->InterfaceReference = HalPnpInterfaceReference;
        PortRangeInterface->InterfaceDereference = HalPnpInterfaceDereference;
        PortRangeInterface->QueryAllocateRange = HalpQueryAllocatePortRange;
        PortRangeInterface->FreeRange = HalpFreePortRange;
        
        return STATUS_SUCCESS;
    }
    else if (IsEqualIID(InterfaceType, &GUID_TRANSLATOR_INTERFACE_STANDARD))
    {
        CM_RESOURCE_TYPE ResourceType = (CM_RESOURCE_TYPE)(ULONG_PTR)InterfaceSpecificData;
        PTRANSLATOR_INTERFACE Tr;

        DPRINT("HalpQueryInterface(GUID_TRANSLATOR_INTERFACE_STANDARD)\n");

        if (ResourceType != CmResourceTypeInterrupt)
        {
            return STATUS_NOT_SUPPORTED;
        }
        if (InterfaceBufferSize < sizeof(TRANSLATOR_INTERFACE))
        {
            return STATUS_BUFFER_TOO_SMALL;
        }

        Tr = (PTRANSLATOR_INTERFACE)Interface;
        Tr->Size = sizeof(TRANSLATOR_INTERFACE);
        Tr->Version = HAL_IRQ_TRANSLATOR_VERSION;
        Tr->Context = UlongToPtr(PCIBus);
        Tr->InterfaceReference = HalPnpInterfaceReference;
        Tr->InterfaceDereference = HalPnpInterfaceDereference;
        Tr->TranslateResources = (PTRANSLATE_RESOURCE_HANDLER)HalIrqTranslateResourcesRoot;
        Tr->TranslateResourceRequirements = (PTRANSLATE_RESOURCE_REQUIREMENTS_HANDLER)HalIrqTranslateResourceRequirementsRoot;
        if (Length) *Length = sizeof(TRANSLATOR_INTERFACE);
        return STATUS_SUCCESS;
    }
    else if (IsEqualIID(InterfaceType, &GUID_INT_ROUTE_INTERFACE_STANDARD))
    {
        PINT_ROUTE_INTERFACE_STANDARD Rt;

        DPRINT1("HalpQueryInterface(GUID_INT_ROUTE_INTERFACE_STANDARD)\n");

        if (InterfaceBufferSize < sizeof(INT_ROUTE_INTERFACE_STANDARD))
        {
            return STATUS_BUFFER_TOO_SMALL;
        }

        Rt = (PINT_ROUTE_INTERFACE_STANDARD)Interface;
        Rt->Size = sizeof(INT_ROUTE_INTERFACE_STANDARD);
        Rt->Version = 1;
        Rt->Context = DeviceObject;
        Rt->InterfaceReference = HalPnpInterfaceReference;
        Rt->InterfaceDereference = HalPnpInterfaceDereference;
        Rt->GetInterruptRouting = (PGET_INTERRUPT_ROUTING)HalpGetInterruptRouting;
        Rt->SetInterruptRoutingToken = (PSET_INTERRUPT_ROUTING_TOKEN)HalpSetInterruptRoutingToken;
        if (Length) *Length = sizeof(INT_ROUTE_INTERFACE_STANDARD);
        return STATUS_SUCCESS;
    }
    else if (IsEqualIID(InterfaceType, &GUID_BUS_INTERFACE_STANDARD))
    {
        PBUS_INTERFACE_STANDARD BusInterface;
        
        DPRINT("HalpQueryInterface(GUID_BUS_INTERFACE_STANDARD)\n");
        
        /* Set the interface size */
        *Length = sizeof(BUS_INTERFACE_STANDARD);
        
        /* Check buffer size */
        if (InterfaceBufferSize < sizeof(BUS_INTERFACE_STANDARD))
        {
            return STATUS_BUFFER_TOO_SMALL;
        }
        
        /* Fill in the interface */
        BusInterface = (PBUS_INTERFACE_STANDARD)Interface;
        BusInterface->Size = sizeof(BUS_INTERFACE_STANDARD);
        BusInterface->Version = 1;
        BusInterface->Context = DeviceObject;
        BusInterface->InterfaceReference = HalPnpInterfaceReference;
        BusInterface->InterfaceDereference = HalPnpInterfaceDereference;
        BusInterface->TranslateBusAddress = (PTRANSLATE_BUS_ADDRESS)HalpTranslateBusAddress;
        BusInterface->GetDmaAdapter = HalpGetDmaAdapter;
        BusInterface->SetBusData = HalpSetBusData;
        BusInterface->GetBusData = HalpGetBusData;
        
        return STATUS_SUCCESS;
    }
    else
    {
        DPRINT1("HalpQueryInterface({%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}) is UNIMPLEMENTED\n",
                InterfaceType->Data1, InterfaceType->Data2, InterfaceType->Data3,
                InterfaceType->Data4[0], InterfaceType->Data4[1],
                InterfaceType->Data4[2], InterfaceType->Data4[3],
                InterfaceType->Data4[4], InterfaceType->Data4[5],
                InterfaceType->Data4[6], InterfaceType->Data4[7]);
    }
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
NTAPI
HalpQueryDeviceRelations(IN PDEVICE_OBJECT DeviceObject,
                         IN DEVICE_RELATION_TYPE RelationType,
                         OUT PDEVICE_RELATIONS* DeviceRelations)
{
    EXTENSION_TYPE ExtensionType;
    PPDO_EXTENSION PdoExtension;
    PFDO_EXTENSION FdoExtension;
    PDEVICE_RELATIONS PdoRelations, FdoRelations;
    PDEVICE_OBJECT* ObjectEntry;
    ULONG i = 0, PdoCount = 0;

    /* Get FDO device extension and PDO count */
    FdoExtension = DeviceObject->DeviceExtension;
    ExtensionType = FdoExtension->ExtensionType;

    /* What do they want? */
    if (RelationType == BusRelations)
    {
        /* This better be an FDO */
        if (ExtensionType == FdoExtensionType)
        {
            /* Count how many PDOs we have */
            PdoExtension = FdoExtension->ChildPdoList;
            while (PdoExtension)
            {
                /* Next one */
                PdoExtension = PdoExtension->Next;
                PdoCount++;
            }

            /* Add the PDOs that already exist in the device relations */
            if (*DeviceRelations)
            {
                PdoCount += (*DeviceRelations)->Count;
            }

            /* Allocate our structure */
            FdoRelations = ExAllocatePoolWithTag(PagedPool,
                                                 FIELD_OFFSET(DEVICE_RELATIONS,
                                                              Objects) +
                                                 sizeof(PDEVICE_OBJECT) * PdoCount,
                                                 TAG_HAL);
            if (!FdoRelations) return STATUS_INSUFFICIENT_RESOURCES;

            /* Save our count */
            FdoRelations->Count = PdoCount;

            /* Query existing relations */
            ObjectEntry = FdoRelations->Objects;
            if (*DeviceRelations)
            {
                /* Check if there were any */
                if ((*DeviceRelations)->Count)
                {
                    /* Loop them all */
                    do
                    {
                        /* Copy into our structure */
                        *ObjectEntry++ = (*DeviceRelations)->Objects[i];
                    }
                    while (++i < (*DeviceRelations)->Count);
                }

                /* Free existing structure */
                ExFreePool(*DeviceRelations);
            }

            /* Now check if we have a PDO list */
            PdoExtension = FdoExtension->ChildPdoList;
            if (PdoExtension)
            {
                /* Loop the PDOs */
                do
                {
                    /* Save our own PDO and reference it */
                    *ObjectEntry++ = PdoExtension->PhysicalDeviceObject;
                    ObReferenceObject(PdoExtension->PhysicalDeviceObject);

                    /* Go to our next PDO */
                    PdoExtension = PdoExtension->Next;
                }
                while (PdoExtension);
            }

            /* Return the new structure */
            *DeviceRelations = FdoRelations;
            return STATUS_SUCCESS;
        }
    }
    else
    {
        /* The only other thing we support is a target relation for the PDO */
        if ((RelationType == TargetDeviceRelation) &&
            (ExtensionType == PdoExtensionType))
        {
            /* Only one entry */
            PdoRelations = ExAllocatePoolWithTag(PagedPool,
                                                 sizeof(DEVICE_RELATIONS),
                                                 TAG_HAL);
            if (!PdoRelations) return STATUS_INSUFFICIENT_RESOURCES;

            /* Fill it out and reference us */
            PdoRelations->Count = 1;
            PdoRelations->Objects[0] = DeviceObject;
            ObReferenceObject(DeviceObject);

            /* Return it */
            *DeviceRelations = PdoRelations;
            return STATUS_SUCCESS;
        }
    }

    /* We don't support anything else */
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
NTAPI
HalpQueryCapabilities(IN PDEVICE_OBJECT DeviceObject,
                      OUT PDEVICE_CAPABILITIES Capabilities)
{
    //PPDO_EXTENSION PdoExtension;
    NTSTATUS Status;
    PAGED_CODE();

    /* Get the extension and check for valid version */
    //PdoExtension = DeviceObject->DeviceExtension;
    ASSERT(Capabilities->Version == 1);
    if (Capabilities->Version == 1)
    {
        /* Can't lock or eject us */
        Capabilities->LockSupported = FALSE;
        Capabilities->EjectSupported = FALSE;

        /* Can't remove or dock us */
        Capabilities->Removable = FALSE;
        Capabilities->DockDevice = FALSE;

        /* Can't access us raw */
        Capabilities->RawDeviceOK = FALSE;

        /* We have a unique ID, and don't bother the user */
        Capabilities->UniqueID = TRUE;
        Capabilities->SilentInstall = TRUE;

        /* Fill out the address */
        Capabilities->Address = InterfaceTypeUndefined;
        Capabilities->UINumber = InterfaceTypeUndefined;

        /* Fill out latencies */
        Capabilities->D1Latency = 0;
        Capabilities->D2Latency = 0;
        Capabilities->D3Latency = 0;

        /* Fill out supported device states */
        Capabilities->DeviceState[PowerSystemWorking] = PowerDeviceD0;
        Capabilities->DeviceState[PowerSystemHibernate] = PowerDeviceD3;
        Capabilities->DeviceState[PowerSystemShutdown] = PowerDeviceD3;
        Capabilities->DeviceState[PowerSystemSleeping3] = PowerDeviceD3;

        /* Done */
        Status = STATUS_SUCCESS;
    }
    else
    {
        /* Fail */
        Status = STATUS_NOT_SUPPORTED;
    }

    /* Return status */
    return Status;
}

NTSTATUS
NTAPI
HalpQueryResources(IN PDEVICE_OBJECT DeviceObject,
                   OUT PCM_RESOURCE_LIST *Resources)
{
    PPDO_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    NTSTATUS Status;
    PCM_RESOURCE_LIST ResourceList;
    PIO_RESOURCE_REQUIREMENTS_LIST RequirementsList;
    PIO_RESOURCE_DESCRIPTOR Descriptor;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR PartialDesc;
    ULONG i;
    PAGED_CODE();

    /* Only the ACPI PDO has requirements */
    if (DeviceExtension->PdoType == AcpiPdo)
    {
        /* Query ACPI requirements */
        Status = HalpQueryAcpiResourceRequirements(&RequirementsList);
        if (!NT_SUCCESS(Status)) return Status;

        ASSERT(RequirementsList->AlternativeLists == 1);

        /* Allocate the resourcel ist */
        ResourceList = ExAllocatePoolWithTag(PagedPool,
                                             sizeof(CM_RESOURCE_LIST),
                                             TAG_HAL);
        if (!ResourceList )
        {
            /* Fail, no memory */
            Status = STATUS_INSUFFICIENT_RESOURCES;
            ExFreePoolWithTag(RequirementsList, TAG_HAL);
            return Status;
        }

        /* Initialize it */
        RtlZeroMemory(ResourceList, sizeof(CM_RESOURCE_LIST));
        ResourceList->Count = 1;

        /* Setup the list fields */
        ResourceList->List[0].BusNumber = -1;
        ResourceList->List[0].InterfaceType = PNPBus;
        ResourceList->List[0].PartialResourceList.Version = 1;
        ResourceList->List[0].PartialResourceList.Revision = 1;
        ResourceList->List[0].PartialResourceList.Count = 0;

        /* Setup the first descriptor */
        PartialDesc = ResourceList->List[0].PartialResourceList.PartialDescriptors;

        /* Find the requirement descriptor for the SCI */
        for (i = 0; i < RequirementsList->List[0].Count; i++)
        {
            /* Get this descriptor */
            Descriptor = &RequirementsList->List[0].Descriptors[i];
            if (Descriptor->Type == CmResourceTypeInterrupt)
            {
                /* Copy requirements descriptor into resource descriptor */
                PartialDesc->Type = CmResourceTypeInterrupt;
                PartialDesc->ShareDisposition = Descriptor->ShareDisposition;
                PartialDesc->Flags = Descriptor->Flags;
                ASSERT(Descriptor->u.Interrupt.MinimumVector ==
                       Descriptor->u.Interrupt.MaximumVector);
                PartialDesc->u.Interrupt.Vector = Descriptor->u.Interrupt.MinimumVector;
                PartialDesc->u.Interrupt.Level = Descriptor->u.Interrupt.MinimumVector;
                PartialDesc->u.Interrupt.Affinity = 0xFFFFFFFF;

                ResourceList->List[0].PartialResourceList.Count++;

                break;
            }
        }

        /* Return resources and success */
        *Resources = ResourceList;

        ExFreePoolWithTag(RequirementsList, TAG_HAL);

        return STATUS_SUCCESS;
    }
    else if (DeviceExtension->PdoType == WdPdo)
    {
        /* Watchdog doesn't */
        return STATUS_NOT_SUPPORTED;
    }
    else
    {
        /* This shouldn't happen */
        return STATUS_UNSUCCESSFUL;
    }
}

NTSTATUS
NTAPI
HalpQueryResourceRequirements(IN PDEVICE_OBJECT DeviceObject,
                              OUT PIO_RESOURCE_REQUIREMENTS_LIST *Requirements)
{
    PPDO_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PAGED_CODE();

    /* Only the ACPI PDO has requirements */
    if (DeviceExtension->PdoType == AcpiPdo)
    {
        /* Query ACPI requirements */
        return HalpQueryAcpiResourceRequirements(Requirements);
    }
    else if (DeviceExtension->PdoType == WdPdo)
    {
        /* Watchdog doesn't */
        return STATUS_NOT_SUPPORTED;
    }
    else
    {
        /* This shouldn't happen */
        return STATUS_UNSUCCESSFUL;
    }
}

NTSTATUS
NTAPI
HalpQueryIdPdo(IN PDEVICE_OBJECT DeviceObject,
               IN BUS_QUERY_ID_TYPE IdType,
               OUT PUSHORT *BusQueryId)
{
    PPDO_EXTENSION PdoExtension;
    PDO_TYPE PdoType;
    PWCHAR CurrentId;
    WCHAR Id[100];
    NTSTATUS Status;
    SIZE_T Length = 0;
    PWCHAR Buffer;

    /* Get the PDO type */
    PdoExtension = DeviceObject->DeviceExtension;
    PdoType = PdoExtension->PdoType;

    /* What kind of ID is being requested? */
    DPRINT("ID: %d\n", IdType);
    switch (IdType)
    {
        case BusQueryDeviceID:
        case BusQueryHardwareIDs:

            /* What kind of PDO is this? */
            if (PdoType == AcpiPdo)
            {
                /* ACPI ID */
                CurrentId = L"ACPI_HAL\\PNP0C08";
                RtlCopyMemory(Id, CurrentId, (wcslen(CurrentId) * sizeof(WCHAR)) + sizeof(UNICODE_NULL));
                Length += (wcslen(CurrentId) * sizeof(WCHAR)) + sizeof(UNICODE_NULL);

                CurrentId = L"*PNP0C08";
                RtlCopyMemory(&Id[wcslen(Id) + 1], CurrentId, (wcslen(CurrentId) * sizeof(WCHAR)) + sizeof(UNICODE_NULL));
                Length += (wcslen(CurrentId) * sizeof(WCHAR)) + sizeof(UNICODE_NULL);
            }
            else if (PdoType == WdPdo)
            {
                /* WatchDog ID */
                CurrentId = L"ACPI_HAL\\PNP0C18";
                RtlCopyMemory(Id, CurrentId, (wcslen(CurrentId) * sizeof(WCHAR)) + sizeof(UNICODE_NULL));
                Length += (wcslen(CurrentId) * sizeof(WCHAR)) + sizeof(UNICODE_NULL);

                CurrentId = L"*PNP0C18";
                RtlCopyMemory(&Id[wcslen(Id) + 1], CurrentId, (wcslen(CurrentId) * sizeof(WCHAR)) + sizeof(UNICODE_NULL));
                Length += (wcslen(CurrentId) * sizeof(WCHAR)) + sizeof(UNICODE_NULL);
            }
            else
            {
                /* Unknown */
                return STATUS_NOT_SUPPORTED;
            }
            break;

        case BusQueryInstanceID:

            /* Instance ID */
            CurrentId = L"0";
            RtlCopyMemory(Id, CurrentId, (wcslen(CurrentId) * sizeof(WCHAR)) + sizeof(UNICODE_NULL));
            Length += (wcslen(CurrentId) * sizeof(WCHAR)) + sizeof(UNICODE_NULL);
            break;

        case BusQueryCompatibleIDs:
        default:

            /* We don't support anything else */
            return STATUS_NOT_SUPPORTED;
    }


    /* Allocate the buffer */
    Buffer = ExAllocatePoolWithTag(PagedPool,
                                   Length + sizeof(UNICODE_NULL),
                                   TAG_HAL);
    if (Buffer)
    {
        /* Copy the string and null-terminate it */
        RtlCopyMemory(Buffer, Id, Length);
        Buffer[Length / sizeof(WCHAR)] = UNICODE_NULL;

        /* Return string */
        *BusQueryId = Buffer;
        Status = STATUS_SUCCESS;
        DPRINT("Returning: %S\n", *BusQueryId);
    }
    else
    {
        /* Fail */
        Status = STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Return status */
    return Status;
}

NTSTATUS
NTAPI
HalpQueryIdFdo(IN PDEVICE_OBJECT DeviceObject,
               IN BUS_QUERY_ID_TYPE IdType,
               OUT PUSHORT *BusQueryId)
{
    NTSTATUS Status;
    SIZE_T Length;
    PWCHAR Id;
    PWCHAR Buffer;

    /* What kind of ID is being requested? */
    DPRINT("ID: %d\n", IdType);
    switch (IdType)
    {
        case BusQueryDeviceID:
        case BusQueryHardwareIDs:

            /* This is our hardware ID */
            Id = HalHardwareIdString;
            break;

        case BusQueryInstanceID:

            /* And our instance ID */
            Id = L"0";
            break;

        default:

            /* We don't support anything else */
            return STATUS_NOT_SUPPORTED;
    }

    /* Calculate the length */
    Length = (wcslen(Id) * sizeof(WCHAR)) + sizeof(UNICODE_NULL);

    /* Allocate the buffer */
    Buffer = ExAllocatePoolWithTag(PagedPool,
                                   Length + sizeof(UNICODE_NULL),
                                   TAG_HAL);
    if (Buffer)
    {
        /* Copy the string and null-terminate it */
        RtlCopyMemory(Buffer, Id, Length);
        Buffer[Length / sizeof(WCHAR)] = UNICODE_NULL;

        /* Return string */
        *BusQueryId = Buffer;
        Status = STATUS_SUCCESS;
        DPRINT("Returning: %S\n", *BusQueryId);
    }
    else
    {
        /* Fail */
        Status = STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Return status */
    return Status;
}

NTSTATUS
NTAPI
HalpDispatchPnp(IN PDEVICE_OBJECT DeviceObject,
                IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStackLocation;
    //PPDO_EXTENSION PdoExtension;
    PFDO_EXTENSION FdoExtension;
    NTSTATUS Status;
    UCHAR Minor;

    /* Get the device extension and stack location */
    FdoExtension = DeviceObject->DeviceExtension;
    IoStackLocation = IoGetCurrentIrpStackLocation(Irp);
    Minor = IoStackLocation->MinorFunction;

    /* FDO? */
    if (FdoExtension->ExtensionType == FdoExtensionType)
    {
        /* Query the IRP type */
        switch (Minor)
        {
            case IRP_MN_QUERY_DEVICE_RELATIONS:

                /* Call the worker */
                DPRINT("Querying device relations for FDO\n");
                Status = HalpQueryDeviceRelations(DeviceObject,
                                                  IoStackLocation->Parameters.QueryDeviceRelations.Type,
                                                  (PVOID)&Irp->IoStatus.Information);
                break;

            case IRP_MN_QUERY_INTERFACE:

                /* Call the worker */
                DPRINT("Querying interface for FDO\n");
                Status = HalpQueryInterface(DeviceObject,
                                            IoStackLocation->Parameters.QueryInterface.InterfaceType,
                                            IoStackLocation->Parameters.QueryInterface.Version,
                                            IoStackLocation->Parameters.QueryInterface.InterfaceSpecificData,
                                            IoStackLocation->Parameters.QueryInterface.Size,
                                            IoStackLocation->Parameters.QueryInterface.Interface,
                                            (PVOID)&Irp->IoStatus.Information);
                break;


            case IRP_MN_QUERY_ID:

                /* Call the worker */
                DPRINT("Querying ID for FDO\n");
                Status = HalpQueryIdFdo(DeviceObject,
                                        IoStackLocation->Parameters.QueryId.IdType,
                                        (PVOID)&Irp->IoStatus.Information);
                break;

            case IRP_MN_QUERY_CAPABILITIES:

                /* Call the worker */
                DPRINT("Querying the capabilities for the FDO\n");
                Status = HalpQueryCapabilities(DeviceObject,
                                               IoStackLocation->Parameters.DeviceCapabilities.Capabilities);
                break;

            default:

                DPRINT("Other IRP: %lx\n", Minor);
                Status = STATUS_NOT_SUPPORTED;
                break;
        }

        /* What happpened? */
        if ((NT_SUCCESS(Status)) || (Status == STATUS_NOT_SUPPORTED))
        {
            /* Set the IRP status, unless this isn't understood */
            if (Status != STATUS_NOT_SUPPORTED)
            {
                Irp->IoStatus.Status = Status;
            }

            /* Pass it on */
            IoSkipCurrentIrpStackLocation(Irp);
            return IoCallDriver(FdoExtension->AttachedDeviceObject, Irp);
        }

        /* Otherwise, we failed, so set the status and complete the request */
        DPRINT1("IRP failed with status: %lx\n", Status);
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }
    else
    {
        /* This is a PDO instead */
        ASSERT(FdoExtension->ExtensionType == PdoExtensionType);
        //PdoExtension = (PPDO_EXTENSION)FdoExtension;
        /* Query the IRP type */
        Status = STATUS_SUCCESS;
        switch (Minor)
        {
            case IRP_MN_START_DEVICE:

                /* We only care about a PCI PDO */
                DPRINT1("Start device received\n");
                /* Complete the IRP normally */
                break;

            case IRP_MN_REMOVE_DEVICE:

                /* Check if this is a PCI device */
                DPRINT1("Remove device received\n");

                /* We're done */
                Status = STATUS_SUCCESS;
                break;

            case IRP_MN_SURPRISE_REMOVAL:

                /* Inherit whatever status we had */
                DPRINT1("Surprise removal IRP\n");
                Status = Irp->IoStatus.Status;
                break;

            case IRP_MN_QUERY_DEVICE_RELATIONS:

                /* Query the device relations */
                DPRINT("Querying PDO relations\n");
                Status = HalpQueryDeviceRelations(DeviceObject,
                                                  IoStackLocation->Parameters.QueryDeviceRelations.Type,
                                                  (PVOID)&Irp->IoStatus.Information);
                break;

            case IRP_MN_QUERY_INTERFACE:

                /* Call the worker */
                DPRINT("Querying interface for PDO\n");
                Status = HalpQueryInterface(DeviceObject,
                                            IoStackLocation->Parameters.QueryInterface.InterfaceType,
                                            IoStackLocation->Parameters.QueryInterface.Version,
                                            IoStackLocation->Parameters.QueryInterface.InterfaceSpecificData,
                                            IoStackLocation->Parameters.QueryInterface.Size,
                                            IoStackLocation->Parameters.QueryInterface.Interface,
                                            (PVOID)&Irp->IoStatus.Information);
                break;

            case IRP_MN_QUERY_CAPABILITIES:

                /* Call the worker */
                DPRINT("Querying the capabilities for the PDO\n");
                Status = HalpQueryCapabilities(DeviceObject,
                                               IoStackLocation->Parameters.DeviceCapabilities.Capabilities);
                break;

            case IRP_MN_QUERY_RESOURCES:

                /* Call the worker */
                DPRINT("Querying the resources for the PDO\n");
                Status = HalpQueryResources(DeviceObject, (PVOID)&Irp->IoStatus.Information);
                break;

            case IRP_MN_QUERY_RESOURCE_REQUIREMENTS:

                /* Call the worker */
                DPRINT("Querying the resource requirements for the PDO\n");
                Status = HalpQueryResourceRequirements(DeviceObject,
                                                       (PVOID)&Irp->IoStatus.Information);
                break;

            case IRP_MN_QUERY_ID:

                /* Call the worker */
                DPRINT("Query the ID for the PDO\n");
                Status = HalpQueryIdPdo(DeviceObject,
                                        IoStackLocation->Parameters.QueryId.IdType,
                                        (PVOID)&Irp->IoStatus.Information);
                break;

            case IRP_MN_QUERY_DEVICE_TEXT:

                /* Device text query */
                DPRINT("Querying device text for PDO\n");
                Status = STATUS_NOT_SUPPORTED;
                break;

            case IRP_MN_FILTER_RESOURCE_REQUIREMENTS:

                /* Filter resource requirements */
                DPRINT("Filtering resource requirements for PDO\n");
                Status = STATUS_NOT_SUPPORTED;
                break;

            case IRP_MN_EJECT:

                /* Eject device */
                DPRINT("Eject request for PDO\n");
                Status = STATUS_NOT_SUPPORTED;
                break;

            case IRP_MN_SET_LOCK:

                /* Set device lock */
                DPRINT("Set lock request for PDO\n");
                Status = STATUS_NOT_SUPPORTED;
                break;

            case IRP_MN_QUERY_PNP_DEVICE_STATE:

                /* Query PnP device state */
                DPRINT("Querying PnP device state for PDO\n");
                Status = STATUS_NOT_SUPPORTED;
                break;

            case IRP_MN_QUERY_BUS_INFORMATION:
            {
                PPNP_BUS_INFORMATION BusInfo;

                DPRINT1("Querying bus information for PDO\n");

                BusInfo = ExAllocatePoolWithTag(PagedPool,
                                                sizeof(PNP_BUS_INFORMATION),
                                                TAG_HAL);
                if (!BusInfo)
                {
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    break;
                }

                BusInfo->BusTypeGuid = GUID_BUS_TYPE_PCI;
                BusInfo->LegacyBusType = PCIBus;
                BusInfo->BusNumber = 0;

                Irp->IoStatus.Information = (ULONG_PTR)BusInfo;
                Status = STATUS_SUCCESS;
                break;
            }

            default:

                /* We don't handle anything else, so inherit the old state */
                DPRINT("Illegal IRP: %lx\n", Minor);
                Status = Irp->IoStatus.Status;
                break;
        }

        /* If it's not supported, inherit the old status */
        if (Status == STATUS_NOT_SUPPORTED) Status = Irp->IoStatus.Status;

        /* Complete the IRP */
        DPRINT("IRP completed with status: %lx\n", Status);
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }
}

NTSTATUS
NTAPI
HalpDispatchWmi(IN PDEVICE_OBJECT DeviceObject,
                IN PIRP Irp)
{
    UNIMPLEMENTED_DBGBREAK("HAL: PnP Driver WMI!\n");
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
HalpDispatchPower(IN PDEVICE_OBJECT DeviceObject,
                  IN PIRP Irp)
{
    PFDO_EXTENSION FdoExtension;

    DPRINT("HAL: PnP Driver Power!\n");
    FdoExtension = DeviceObject->DeviceExtension;
    if (FdoExtension->ExtensionType == FdoExtensionType)
    {
        PoStartNextPowerIrp(Irp);
        IoSkipCurrentIrpStackLocation(Irp);
        return PoCallDriver(FdoExtension->AttachedDeviceObject, Irp);
    }
    else
    {
        PoStartNextPowerIrp(Irp);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }
}

NTSTATUS
NTAPI
HalpDriverEntry(IN PDRIVER_OBJECT DriverObject,
                IN PUNICODE_STRING RegistryPath)
{
    NTSTATUS Status;
    PDEVICE_OBJECT TargetDevice = NULL;

    DPRINT("HAL: PnP Driver ENTRY!\n");

    /* This is us */
    HalpDriverObject = DriverObject;

    /* Set up add device */
    DriverObject->DriverExtension->AddDevice = HalpAddDevice;

    /* Set up the callouts */
    DriverObject->MajorFunction[IRP_MJ_PNP] = HalpDispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = HalpDispatchPower;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = HalpDispatchWmi;

    /* Create the PDO and tell the PnP manager about us*/
    Status = IoReportDetectedDevice(DriverObject,
                                    InterfaceTypeUndefined,
                                    -1,
                                    -1,
                                    NULL,
                                    NULL,
                                    FALSE,
                                    &TargetDevice);
    if (!NT_SUCCESS(Status))
        return Status;

    TargetDevice->Flags &= ~DO_DEVICE_INITIALIZING;

    /* Set up the device stack */
    Status = HalpAddDevice(DriverObject, TargetDevice);
    if (!NT_SUCCESS(Status))
    {
        IoDeleteDevice(TargetDevice);
        return Status;
    }

    /* Return to kernel */
    return Status;
}

NTSTATUS
NTAPI
HaliInitPnpDriver(VOID)
{
    NTSTATUS Status;
    UNICODE_STRING DriverString;
    PAGED_CODE();

    /* Create the driver */
    RtlInitUnicodeString(&DriverString, L"\\Driver\\ACPI_HAL");
    Status = IoCreateDriver(&DriverString, HalpDriverEntry);

    /* Return status */
    return Status;
}

/* EOF */
