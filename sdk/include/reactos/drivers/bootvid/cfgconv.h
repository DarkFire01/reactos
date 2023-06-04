/*
 * PROJECT:     ReactOS Boot Video Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Conversion helpers between legacy video/monitor configuration
 *              data structures, and newer ones compatible with CM_PARTIAL_RESOURCE_LIST
 *              resource descriptors.
 * COPYRIGHT:   Copyright 2023 Hermès Bélusca-Maïto <hermes.belusca-maito@reactos.org>
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * NOTE: Legacy video display adapter and monitor peripheral configuration
 * data is specified with the VIDEO_HARDWARE_CONFIGURATION_DATA (see video.h
 * -- with data starting at the 'Version' field and following) and the
 * MONITOR_CONFIGURATION_DATA structures (see arc.h), respectively, with
 * Version == 0 or 1, Revision == 0.
 *
 * New configuration data is provided with a CM_PARTIAL_RESOURCE_LIST.
 * The Irql/Vector interrupt settings are now provided with a
 * CmResourceTypeInterrupt resource descriptor. Both ControlBase/Size
 * and CursorBase/Size memory I/O ports are provided with successive
 * CmResourceTypePort resource descriptors (respectively in this order),
 * while the framebuffer's FrameBase/Size
 * is provided (in a 64-bit compatible way) with a CmResourceTypeMemory
 * resource descriptor.
 * Extended video/framebuffer-specific data is specified with the
 * ReactOS-specific CM_FRAMEBUF_DEVICE_DATA structure (see below),
 * while extended monitor configuration data is provided with a
 * CM_MONITOR_DEVICE_DATA structure.
 */


#ifndef _BOOTVID_FRAMEBUF_

/*
 * Note: The DDK video.h VIDEO_HARDWARE_CONFIGURATION_DATA structure
 * has a similar layout, where:
 * - The first two fields, InterfaceType and BusNumber, are common
 *   with the CM_FULL_RESOURCE_DESCRIPTOR header;
 * - The Version and Revision fields correspond to the first two
 *   fields of CM_PARTIAL_RESOURCE_LIST.
 *
 * The other fields are of legacy layout, instead of the newer
 * CM_PARTIAL_RESOURCE_LIST one.
 */
#include <pshpack1.h>
typedef struct _MONITOR_HARDWARE_CONFIGURATION_DATA
{
    INTERFACE_TYPE InterfaceType;
    ULONG BusNumber;
    MONITOR_CONFIGURATION_DATA;
} MONITOR_HARDWARE_CONFIGURATION_DATA, *PMONITOR_HARDWARE_CONFIGURATION_DATA;
#include <poppack.h>

#endif // _BOOTVID_FRAMEBUF_


/**
 * @brief
 * Given a CM_PARTIAL_RESOURCE_LIST, obtain pointers to resource descriptors
 * for legacy video configuration: interrupt, control and cursor I/O ports,
 * and framebuffer memory descriptors. In addition, retrieve any
 * device-specific resource present.
 **/
FORCEINLINE
VOID
GetVideoData(
    _In_ PCM_PARTIAL_RESOURCE_LIST ResourceList,
    _Out_opt_ PCM_PARTIAL_RESOURCE_DESCRIPTOR* Interrupt,
    _Out_opt_ PCM_PARTIAL_RESOURCE_DESCRIPTOR* ControlPort,
    _Out_opt_ PCM_PARTIAL_RESOURCE_DESCRIPTOR* CursorPort,
    _Out_ PCM_PARTIAL_RESOURCE_DESCRIPTOR* FrameBuffer,
    _Out_opt_ PCM_PARTIAL_RESOURCE_DESCRIPTOR* DeviceSpecific)
{
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;
    ULONG PortCount = 0, IntCount = 0, MemCount = 0;
    ULONG i;

    /* Initialize the return values */
    if (Interrupt)   *Interrupt   = NULL;
    if (ControlPort) *ControlPort = NULL;
    if (CursorPort)  *CursorPort  = NULL;
    *FrameBuffer = NULL;
    if (DeviceSpecific) *DeviceSpecific = NULL;

    for (i = 0; i < ResourceList->Count; ++i)
    {
        Descriptor = &ResourceList->PartialDescriptors[i];

        switch (Descriptor->Type)
        {
            case CmResourceTypePort:
            {
                /* We only check for memory I/O ports */
                // if (!(Descriptor->Flags & CM_RESOURCE_PORT_MEMORY))
                if (Descriptor->Flags & CM_RESOURCE_PORT_IO)
                    break;

                /* If more than two memory I/O ports
                 * have been encountered, ignore them */
                if (PortCount > 2)
                    break;
                ++PortCount;

                /* First port is Control; Second port is Cursor */
                if (PortCount == 1)
                {
                    // Descriptor->u.Port;
                    if (ControlPort)
                        *ControlPort = Descriptor;
                }
                else // if (PortCount == 2)
                {
                    // Descriptor->u.Port;
                    if (CursorPort)
                        *CursorPort = Descriptor;
                }
                break;
            }

            case CmResourceTypeInterrupt:
            {
                /* If more than one interrupt resource
                 * has been encountered, ignore them */
                if (IntCount > 1)
                    break;
                ++IntCount;

                // Descriptor->u.Interrupt;
                if (Interrupt)
                    *Interrupt = Descriptor;
                break;
            }

            case CmResourceTypeMemory:
            {
                /* If more than one memory resource
                 * has been encountered, ignore them */
                if (MemCount > 1)
                    break;
                ++MemCount;

                // or CM_RESOURCE_MEMORY_WRITE_ONLY ??
                // if (!(Descriptor->Flags & CM_RESOURCE_MEMORY_READ_WRITE))
                // {
                //     /* Cannot use this framebuffer */
                //     break;
                // }

                // Descriptor->u.Memory;
                *FrameBuffer = Descriptor;
                break;
            }

            case CmResourceTypeDeviceSpecific:
            {
                /* NOTE: This descriptor *MUST* be the last one.
                 * The actual device data follows the descriptor. */
                ASSERT(i == ResourceList->Count - 1);
                i = ResourceList->Count; // To force-break the for-loop.

                if (DeviceSpecific)
                    *DeviceSpecific = Descriptor;
                break;
            }
        }
    }
}

static inline
VOID
DoConvertVideoDataToLegacyConfigData(
    _Inout_ PVIDEO_HARDWARE_CONFIGURATION_DATA configData,
    _In_opt_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Interrupt,
    _In_opt_ PCM_PARTIAL_RESOURCE_DESCRIPTOR ControlPort,
    _In_opt_ PCM_PARTIAL_RESOURCE_DESCRIPTOR CursorPort,
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR FrameBuffer)
{
    if (Interrupt)
    {
        if ((Interrupt->u.Interrupt.Level & 0xFFFF0000) != 0)
        {
            DPRINT1("WARNING: Interrupt Level %lu truncated to 16 bits!\n",
                    Interrupt->u.Interrupt.Level);
        }
        configData->Irql   = (USHORT)Interrupt->u.Interrupt.Level;
        configData->Vector = Interrupt->u.Interrupt.Vector;
    }

    if (ControlPort)
    {
        if (ControlPort->u.Port.Start.HighPart != 0)
        {
            DPRINT1("WARNING: Port %I64u truncated to 32 bits!\n",
                    ControlPort->u.Port.Start.QuadPart);
        }
        configData->ControlBase = ControlPort->u.Port.Start.LowPart;
        configData->ControlSize = ControlPort->u.Port.Length;
    }

    if (CursorPort)
    {
        if (CursorPort->u.Port.Start.HighPart != 0)
        {
            DPRINT1("WARNING: Port %I64u truncated to 32 bits!\n",
                    CursorPort->u.Port.Start.QuadPart);
        }
        configData->CursorBase = CursorPort->u.Port.Start.LowPart;
        configData->CursorSize = CursorPort->u.Port.Length;
    }

    if (FrameBuffer)
    {
        if (FrameBuffer->u.Memory.Start.HighPart != 0)
        {
            DPRINT1("WARNING: Memory %I64u truncated to 32 bits!\n",
                    FrameBuffer->u.Memory.Start.QuadPart);
        }
        configData->FrameBase = FrameBuffer->u.Memory.Start.LowPart;
        configData->FrameSize = FrameBuffer->u.Memory.Length;
    }
}

/**
 * @brief
 * Convert video resource descriptor data into legacy video configuration.
 **/
FORCEINLINE
VOID
ConvertVideoDataToLegacyConfigData(
    _Inout_ PVIDEO_HARDWARE_CONFIGURATION_DATA configData,
    _In_ PCM_FULL_RESOURCE_DESCRIPTOR cmDescriptor)
{
    /* New format: heuristically convert it back
     * to the old one and update our pointers. */
    PCM_PARTIAL_RESOURCE_LIST ResourceList = &cmDescriptor->PartialResourceList;

    PCM_PARTIAL_RESOURCE_DESCRIPTOR Interrupt;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR ControlPort;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR CursorPort;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR FrameBuffer;

    configData->InterfaceType = cmDescriptor->InterfaceType;
    configData->BusNumber     = cmDescriptor->BusNumber;
    configData->Version  = ResourceList->Version;
    configData->Revision = ResourceList->Revision;

    // if (!Entry->ConfigurationData ||
    //     Entry->ComponentEntry.ConfigurationDataLength < sizeof(CM_PARTIAL_RESOURCE_LIST))
    // {
    //     /* Invalid entry?! */
    //     return ERROR_INVALID_PARAMETER;
    // }

    GetVideoData(ResourceList,
                 &Interrupt,
                 &ControlPort,
                 &CursorPort,
                 &FrameBuffer,
                 NULL);

    DoConvertVideoDataToLegacyConfigData(configData,
                                         Interrupt,
                                         ControlPort,
                                         CursorPort,
                                         FrameBuffer);
}

/**
 * @brief
 * Convert legacy video configuration data into video resource descriptor.
 **/
FORCEINLINE
VOID
ConvertLegacyVideoConfigDataToDeviceData(
    _In_ PVIDEO_HARDWARE_CONFIGURATION_DATA configData,
    _Out_opt_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Interrupt,
    _Out_opt_ PCM_PARTIAL_RESOURCE_DESCRIPTOR ControlPort,
    _Out_opt_ PCM_PARTIAL_RESOURCE_DESCRIPTOR CursorPort,
    _Out_ PCM_PARTIAL_RESOURCE_DESCRIPTOR FrameBuffer)
{
    // cmDescriptor->InterfaceType = configData->InterfaceType;
    // cmDescriptor->BusNumber     = configData->BusNumber;
    // ResourceList->Version  = configData->Version;
    // ResourceList->Revision = configData->Revision;

    if (Interrupt)
    {
        Interrupt->Type = CmResourceTypeInterrupt;

        Interrupt->u.Interrupt.Level  = configData->Irql;
        Interrupt->u.Interrupt.Vector = configData->Vector;
    }

    if (ControlPort)
    {
        ControlPort->Type = CmResourceTypePort;

        /* We only check for memory I/O ports */
        ControlPort->Flags &= ~CM_RESOURCE_PORT_IO;
        ControlPort->Flags |= CM_RESOURCE_PORT_MEMORY;

        ControlPort->u.Port.Start.HighPart = 0;
        ControlPort->u.Port.Start.LowPart = configData->ControlBase;
        ControlPort->u.Port.Length = configData->ControlSize;
    }

    if (CursorPort)
    {
        CursorPort->Type = CmResourceTypePort;

        /* We only check for memory I/O ports */
        CursorPort->Flags &= ~CM_RESOURCE_PORT_IO;
        CursorPort->Flags |= CM_RESOURCE_PORT_MEMORY;

        CursorPort->u.Port.Start.HighPart = 0;
        CursorPort->u.Port.Start.LowPart = configData->CursorBase;
        CursorPort->u.Port.Length = configData->CursorSize;
    }

    FrameBuffer->Type = CmResourceTypeMemory;

    // FrameBuffer->Flags |= CM_RESOURCE_MEMORY_READ_WRITE;
    // or CM_RESOURCE_MEMORY_WRITE_ONLY ??

    FrameBuffer->u.Memory.Start.HighPart = 0;
    FrameBuffer->u.Memory.Start.LowPart = configData->FrameBase;
    FrameBuffer->u.Memory.Length = configData->FrameSize;
}


/**
 * @brief
 * Given a CM_PARTIAL_RESOURCE_LIST, obtain a pointer to resource descriptor
 * for monitor configuration data, listed as a device-specific resource.
 **/
FORCEINLINE
VOID
GetMonitorData(
    _In_ PCM_PARTIAL_RESOURCE_LIST ResourceList,
    _Out_ PCM_PARTIAL_RESOURCE_DESCRIPTOR* DeviceSpecific)
{
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;
    ULONG i;

    /* Initialize the return values */
    *DeviceSpecific = NULL;

    /* Find the CmResourceTypeDeviceSpecific CM_MONITOR_DEVICE_DATA */
    for (i = 0; i < ResourceList->Count; ++i)
    {
        Descriptor = &ResourceList->PartialDescriptors[i];
        if (Descriptor->Type == CmResourceTypeDeviceSpecific)
        {
            /* NOTE: This descriptor *MUST* be the last one.
             * The actual device data follows the descriptor. */
            // PCM_MONITOR_DEVICE_DATA MonitorData = (PCM_MONITOR_DEVICE_DATA)(Descriptor + 1);

            ASSERT(i == ResourceList->Count - 1);

            if (DeviceSpecific)
                *DeviceSpecific = Descriptor;
            break; // Exit the for-loop.
        }
    }
}

static inline
VOID
DoConvertMonitorDataToLegacyConfigData(
    _Inout_ PMONITOR_HARDWARE_CONFIGURATION_DATA configData,
    _In_ PCM_MONITOR_DEVICE_DATA MonitorData)
{
    configData->HorizontalResolution = MonitorData->HorizontalResolution;
    if (MonitorData->HorizontalDisplayTimeHigh != 0)
    {
        DPRINT1("WARNING: Monitor HorizontalDisplayTime truncated to 16 bits!\n");
        configData->HorizontalDisplayTime = MonitorData->HorizontalDisplayTimeLow;
    }
    else
    {
        configData->HorizontalDisplayTime = MonitorData->HorizontalDisplayTime;
    }
    if (MonitorData->HorizontalBackPorchHigh != 0)
    {
        DPRINT1("WARNING: Monitor HorizontalBackPorch truncated to 16 bits!\n");
        configData->HorizontalBackPorch = MonitorData->HorizontalBackPorchLow;
    }
    else
    {
        configData->HorizontalBackPorch = MonitorData->HorizontalBackPorch;
    }
    if (MonitorData->HorizontalFrontPorchHigh != 0)
    {
        DPRINT1("WARNING: Monitor HorizontalFrontPorch truncated to 16 bits!\n");
        configData->HorizontalFrontPorch = MonitorData->HorizontalFrontPorchLow;
    }
    else
    {
        configData->HorizontalFrontPorch = MonitorData->HorizontalFrontPorch;
    }
    if (MonitorData->HorizontalSyncHigh != 0)
    {
        DPRINT1("WARNING: Monitor HorizontalSync truncated to 16 bits!\n");
        configData->HorizontalSync = MonitorData->HorizontalSyncLow;
    }
    else
    {
        configData->HorizontalSync = MonitorData->HorizontalSync;
    }

    configData->VerticalResolution = MonitorData->VerticalResolution;
    if (MonitorData->VerticalBackPorchHigh != 0)
    {
        DPRINT1("WARNING: Monitor VerticalBackPorch truncated to 16 bits!\n");
        configData->VerticalBackPorch = MonitorData->VerticalBackPorchLow;
    }
    else
    {
        configData->VerticalBackPorch = MonitorData->VerticalBackPorch;
    }
    if (MonitorData->VerticalFrontPorchHigh != 0)
    {
        DPRINT1("WARNING: Monitor VerticalFrontPorch truncated to 16 bits!\n");
        configData->VerticalFrontPorch = MonitorData->VerticalFrontPorchLow;
    }
    else
    {
        configData->VerticalFrontPorch = MonitorData->VerticalFrontPorch;
    }
    if (MonitorData->VerticalSyncHigh != 0)
    {
        DPRINT1("WARNING: Monitor VerticalSync truncated to 16 bits!\n");
        configData->VerticalSync = MonitorData->VerticalSyncLow;
    }
    else
    {
        configData->VerticalSync = MonitorData->VerticalSync;
    }

    configData->HorizontalScreenSize = MonitorData->HorizontalScreenSize;
    configData->VerticalScreenSize = MonitorData->VerticalScreenSize;
}

FORCEINLINE
VOID
ConvertMonitorDataToLegacyConfigData(
    _Inout_ PMONITOR_HARDWARE_CONFIGURATION_DATA configData,
    _In_ PCM_FULL_RESOURCE_DESCRIPTOR cmDescriptor)
{
    /* New format: heuristically convert it back
     * to the old one and update our pointers. */
    PCM_PARTIAL_RESOURCE_LIST ResourceList = &cmDescriptor->PartialResourceList;

    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;

    configData->InterfaceType = cmDescriptor->InterfaceType;
    configData->BusNumber     = cmDescriptor->BusNumber;
    configData->Version  = ResourceList->Version;
    configData->Revision = ResourceList->Revision;

    // if (!Entry->ConfigurationData ||
    //     Entry->ComponentEntry.ConfigurationDataLength < sizeof(CM_PARTIAL_RESOURCE_LIST))
    // {
    //     /* Invalid entry?! */
    //     return ERROR_INVALID_PARAMETER;
    // }

    GetMonitorData(ResourceList, &Descriptor);

    if (Descriptor)
    {
        /* NOTE: This descriptor *MUST* be the last one.
         * The actual device data follows the descriptor. */
        PCM_MONITOR_DEVICE_DATA MonitorData = (PCM_MONITOR_DEVICE_DATA)(Descriptor + 1);
        DoConvertMonitorDataToLegacyConfigData(configData, MonitorData);
    }
}

FORCEINLINE
VOID
ConvertLegacyMonitorConfigDataToDeviceData(
    _In_ PMONITOR_HARDWARE_CONFIGURATION_DATA configData,
    _Out_ PCM_MONITOR_DEVICE_DATA MonitorData)
{
    // cmDescriptor->InterfaceType = configData->InterfaceType;
    // cmDescriptor->BusNumber     = configData->BusNumber;

    MonitorData->Version = configData->Version;
    MonitorData->Revision = configData->Revision;
    MonitorData->HorizontalScreenSize = configData->HorizontalScreenSize;
    MonitorData->VerticalScreenSize = configData->VerticalScreenSize;
    MonitorData->HorizontalResolution = configData->HorizontalResolution;
    MonitorData->VerticalResolution = configData->VerticalResolution;
    MonitorData->HorizontalDisplayTimeLow = 0;
    MonitorData->HorizontalDisplayTime = configData->HorizontalDisplayTime;
    MonitorData->HorizontalDisplayTimeHigh = 0;
    MonitorData->HorizontalBackPorchLow = 0;
    MonitorData->HorizontalBackPorch = configData->HorizontalBackPorch;
    MonitorData->HorizontalBackPorchHigh = 0;
    MonitorData->HorizontalFrontPorchLow = 0;
    MonitorData->HorizontalFrontPorch = configData->HorizontalFrontPorch;
    MonitorData->HorizontalFrontPorchHigh = 0;
    MonitorData->HorizontalSyncLow = 0;
    MonitorData->HorizontalSync = configData->HorizontalSync;
    MonitorData->HorizontalSyncHigh = 0;
    MonitorData->VerticalBackPorchLow = 0;
    MonitorData->VerticalBackPorch = configData->VerticalBackPorch;
    MonitorData->VerticalBackPorchHigh = 0;
    MonitorData->VerticalFrontPorchLow = 0;
    MonitorData->VerticalFrontPorch = configData->VerticalFrontPorch;
    MonitorData->VerticalFrontPorchHigh = 0;
    MonitorData->VerticalSyncLow = 0;
    MonitorData->VerticalSync = configData->VerticalSync;
    MonitorData->VerticalSyncHigh = 0;
}

#ifdef __cplusplus
}
#endif

/* EOF */
