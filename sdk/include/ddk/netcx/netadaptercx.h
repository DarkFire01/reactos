/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NetAdapterCx client driver interface
 */

#pragma once

#include <ndis.h>
#include <wdf.h>

#include <net/extension.h>
#include <net/fragment.h>
#include <net/packet.h>
#include <net/ring.h>
#include <net/ringcollection.h>

#include <netcx/netadaptercxtypes.h>
#include <netcx/netfuncenum.h>
#include <netcx/netdevice.h>
#include <netcx/netadapterpacket.h>
#include <netcx/nettxqueue.h>
#include <netcx/netrxqueue.h>
#include <netcx/netadapter.h>
#include <netcx/netadapteroffload.h>
#include <netcx/netreceivefilter.h>
#include <netcx/netpoweroffload.h>
#include <netcx/netpoweroffloadlist.h>
#include <netcx/netwakesource.h>
#include <netcx/netwakesourcelist.h>
#include <netcx/netconfiguration.h>
