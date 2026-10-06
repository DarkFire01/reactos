/*
 * PROJECT:     ReactOS USB4 Host Router Driver
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

#include "usb4hrutil.h"
#include "usb4hrhw.h"

class Usb4HrHostRouter;
class Usb4HrHardware;
class Usb4HrInterrupts;
class Usb4HrRingZero;
class Usb4HrConfigAccessor;
class Usb4HrTopology;
class Usb4HrTunnelManager;
class Usb4HrPowerPdo;
class Usb4HrRootRouter;

/** Route string: Port[n] is the downstream adapter taken at depth n + 1. Depth 0 is the host router. */
struct Usb4HrRoute
{
    UCHAR Depth;
    UCHAR Port[USB4HR_MAX_DEPTH];
};

/* Raw device class of the virtual power coordination PDO */
DEFINE_GUID(GUID_USB4HR_POWER_PDO_CLASS,
    0x923c1977, 0x954a, 0x4b41, 0xb4, 0x8d, 0xc8, 0xb4, 0x3d, 0xc8, 0x12, 0x69);

#include "driver.h"
#include "hardware.h"
#include "interrupts.h"
#include "ring.h"
#include "configaccess.h"
#include "topology.h"
#include "tunnel.h"
#include "powerpdo.h"
#include "rootrouter.h"
#include "hostrouter.h"

/* driver.cpp */
extern "C" DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_UNLOAD Usb4HrEvtDriverUnload;

/* hostrouter.cpp */
EVT_WDF_DRIVER_DEVICE_ADD Usb4HrEvtDeviceAdd;
