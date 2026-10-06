/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Master header; every source file includes it first
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include <devpropdef.h>
#include <reactos/drivers/usb4/usb4hrd.h>

#include "usb4drutil.h"
#include "usb4drhw.h"

class Usb4DrFdo;
class Usb4DrHostLink;
class Usb4DrRouter;
class Usb4DrAdapter;
class Usb4DrRouterOps;
class Usb4DrDrom;
class Usb4DrPortSet;
class Usb4DrPort;
class Usb4DrEventPump;
class Usb4DrTunnels;
class Usb4DrProtocolAdapter;
class Usb4DrSegments;
class Usb4DrChildPdo;
class Usb4DrForwarder;
class Usb4DrParentLink;

/* Largest adapter number of a router (ROUTER_CS_1 max adapter is 6 bits) */
#define USB4DR_MAX_ADAPTERS             64

/* Adapter numbers of one router */
#define USB4DR_MAX_PORTS                (USB4DR_MAX_ADAPTERS / 2)

/* Raw device class of a router this driver cannot enumerate */
DEFINE_GUID(GUID_USB4DR_UNKNOWN_ROUTER_CLASS,
    0x1f7ce3d8, 0xe217, 0x4988, 0x9f, 0xbd, 0xe8, 0x9d, 0xa4, 0xd0, 0x25, 0x8d);

/** What a downstream port learns about the router plugged into it; handed to the child PDO. */
struct Usb4DrRouterIdentity
{
    USHORT VendorId;                /**< ROUTER_CS_0 */
    USHORT ProductId;               /**< ROUTER_CS_0 */
    UCHAR Revision;                 /**< ROUTER_CS_1 revision */
    UCHAR Lane0AdapterNumber;       /**< the parent's downstream lane 0 adapter, also the instance ID */
    UCHAR UfpLane0AdapterNumber;    /**< the child's upstream lane 0 adapter */
    UCHAR Usb4Version;              /**< ROUTER_CS_4 USB4 version byte */
    UCHAR MaxAdapterNumber;         /**< ROUTER_CS_1 max adapter */
    BOOLEAN IsKnown;                /**< FALSE: report a raw unknown router */
    BOOLEAN IsTbt3;                 /**< USB4 version below 0x20 */
    UCHAR Reserved;
    ULONG64 RouterUuid;             /**< ROUTER_CS_2/3 or the TBT3 ID */
    ULONG CurrentLinkSpeed;         /**< lane adapter current link speed of the DFP */
};

/** Child kinds reported through the child lists. */
enum class Usb4DrChildType : ULONG
{
    DeviceRouter = 0,
    InterDomain = 1
};

/** Identification description of one child list entry. */
struct Usb4DrChildDescription
{
    WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER Header;
    Usb4DrChildType Type;
    ULONG Generation;               /**< connect generation of the port when reported */
    Usb4DrRouterIdentity Identity;
};

#include "driver.h"
#include "hostlink.h"
#include "drom.h"
#include "routerop.h"
#include "adapter.h"
#include "router.h"
#include "eventpump.h"
#include "port.h"
#include "segments.h"
#include "protoadapter.h"
#include "forward.h"
#include "pdo.h"
#include "fdo.h"

/* driver.cpp */
extern "C" DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_UNLOAD Usb4DrEvtDriverUnload;

/* fdo.cpp */
EVT_WDF_DRIVER_DEVICE_ADD Usb4DrEvtDeviceAdd;
