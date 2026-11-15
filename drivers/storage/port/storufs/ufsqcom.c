/*
 * PROJECT:     ReactOS UFS Host Controller Miniport Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Qualcomm host controller setup around link startup
 * COPYRIGHT:   Justin Miller (justinmiller100@gmail.com)
 */

#include "storufs.h"

#define NDEBUG
#include <debug.h>

/* Qualcomm vendor registers in the host controller block */
#define QCOM_UFS_SYS1CLK_1US        0xC0
#define QCOM_UFS_TX_SYMBOL_CLK      0xC4
#define QCOM_UFS_LINK_STARTUP_TIMER 0xD8
#define QCOM_UFS_HW_VERSION         0xE4
#define QCOM_UFS_PHY_TX_LANE_ENABLE 0x3744

/* UniPro attributes */
#define QCOM_MPHY_TX_FSM_STATE      0x0041
#define QCOM_PA_CONNECTED_TX_LANES  0x1540
#define QCOM_PA_LOCAL_TX_LCC_ENABLE 0x155E
#define QCOM_PA_PEER_TX_LCC_ENABLE  0x155F

#define QCOM_TX_FSM_HIBERN8         1
#define QCOM_FSM_POLLS              10
#define QCOM_CORE_CLOCK_MHZ         200
#define QCOM_TX_SYMBOL_PWM          0x1401
#define QCOM_LINK_STARTUP_TICKS     20000000

static
BOOLEAN
UfsDmeGet(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ USHORT Attribute,
    _Out_ PULONG Value)
{
    if (!NT_SUCCESS(UfsSendUic(Adapter, UFS_UIC_DME_GET, (ULONG)Attribute << 16, 0, 0, NULL)))
        return FALSE;

    *Value = UfsRead(Adapter, UFS_REG_UCMDARG3);
    return TRUE;
}

static
BOOLEAN
UfsDmeSet(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ USHORT Attribute,
    _In_ ULONG Value)
{
    return NT_SUCCESS(UfsSendUic(Adapter, UFS_UIC_DME_SET, (ULONG)Attribute << 16, 0, Value, NULL));
}

/**
 * @brief Tells whether the controller is a Qualcomm one, from the hardware ID of its PDO.
 */
BOOLEAN
UfsIsQualcommController(
    _In_ PUFS_ADAPTER_EXTENSION Adapter)
{
    PDEVICE_OBJECT AdapterDevice = NULL, Pdo = NULL, Lower = NULL;
    WCHAR HardwareId[64];
    ULONG Length = 0;

    if (StorPortExtendedFunction(ExtFunctionGetDeviceObjects, Adapter, &AdapterDevice, &Pdo, &Lower) !=
        STOR_STATUS_SUCCESS || Pdo == NULL)
    {
        return FALSE;
    }

    RtlZeroMemory(HardwareId, sizeof(HardwareId));
    if (!NT_SUCCESS(IoGetDeviceProperty(Pdo, DevicePropertyHardwareID,
                                        sizeof(HardwareId) - sizeof(WCHAR), HardwareId, &Length)))
    {
        return FALSE;
    }

    return (_wcsnicmp(HardwareId, L"ACPI\\QCOM", 9) == 0) ||
           (_wcsnicmp(HardwareId, L"ACPI\\VEN_QCOM", 13) == 0);
}

/**
 * @brief Before link startup: the PHY settles in hibern8, the timers are clocked
 * for the core clock on older controllers, and line control commands go off.
 */
VOID
UfsQcomBeforeLinkStartup(
    _In_ PUFS_ADAPTER_EXTENSION Adapter)
{
    ULONG Poll, State = 0, Version;

    for (Poll = 0; Poll < QCOM_FSM_POLLS; Poll++)
    {
        if (UfsDmeGet(Adapter, QCOM_MPHY_TX_FSM_STATE, &State) && State == QCOM_TX_FSM_HIBERN8)
            break;
        StorPortStallExecution(1);
    }

    Version = UfsRead(Adapter, QCOM_UFS_HW_VERSION);
    DPRINT1("UFS: Qualcomm HW 0x%08lx, TX FSM %lu\n", Version, State);

    if ((Version >> 16) < 0x4000)
    {
        UfsWrite(Adapter, QCOM_UFS_SYS1CLK_1US, QCOM_CORE_CLOCK_MHZ);
        UfsWrite(Adapter, QCOM_UFS_TX_SYMBOL_CLK, QCOM_TX_SYMBOL_PWM);
        UfsWrite(Adapter, QCOM_UFS_LINK_STARTUP_TIMER, QCOM_LINK_STARTUP_TICKS);
    }

    UfsDmeSet(Adapter, QCOM_PA_LOCAL_TX_LCC_ENABLE, 0);
    UfsDmeSet(Adapter, QCOM_PA_PEER_TX_LCC_ENABLE, 0);
}

/**
 * @brief After link startup: the PHY drives exactly the lanes the link came up with.
 */
VOID
UfsQcomAfterLinkStartup(
    _In_ PUFS_ADAPTER_EXTENSION Adapter)
{
    ULONG Lanes = 0;

    /* A failed read leaves no lanes, the same as the mask Windows writes then */
    if (!UfsDmeGet(Adapter, QCOM_PA_CONNECTED_TX_LANES, &Lanes) || Lanes > 4)
        Lanes = 0;

    UfsWrite(Adapter, QCOM_UFS_PHY_TX_LANE_ENABLE, (1 << Lanes) - 1);
    DPRINT1("UFS: Qualcomm TX lanes %lu enabled\n", Lanes);
}
