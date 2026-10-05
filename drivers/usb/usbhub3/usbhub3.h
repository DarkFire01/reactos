/*
 * PROJECT:     ReactOS SuperSpeed USB Hub Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Common declarations for usbhub3
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

#include <ntddk.h>
#include <windef.h>
#include <ntstrsafe.h>
#include <wdmguid.h>
#include <devpropdef.h>
#include <devpkey.h>
#include <wdf.h>
#include <usb.h>
#include <usbioctl.h>
#include <usbbusif.h>
extern "C" {
#include <usbdlib.h>
}
#include <ucxclass.h>
#include <drivers/usb3/hubucx.h>
#include <drivers/usb3/usbdclient.h>
#include <drivers/usb3/usbdhub.h>

#include "hubguid.h"
#include "hubutil.h"
#include "hubdriver.h"
#include "descvalidation.h"

/* Objects that own the state machines */
class HubFdo;
class HubPort;
class HubChild;
class HubPdo;

#include "hsm.h"
#include "psm.h"
#include "dsm.h"
#include "ism.h"

#include "hubxfer.h"
#include "hubfdo.h"
#include "hubport.h"
#include "hubchild.h"
#include "hubid.h"
#include "devucx.h"
#include "hubpdo.h"
#include "hubsvc.h"
