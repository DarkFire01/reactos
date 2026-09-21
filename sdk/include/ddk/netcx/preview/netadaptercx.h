/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NetAdapterCx client driver interface, preview surface
 *
 * The preview headers are the in development half of the API: transmit
 * demultiplexing, execution contexts, and the execution context member of
 * NET_PACKET_QUEUE_CONFIG, which NETCX_ADAPTER_PREVIEW turns on.
 */

#pragma once

#define NETCX_ADAPTER_PREVIEW

#include <netcx/netadaptercx.h>
#include <netcx/preview/netadapter.h>
#include <netcx/preview/netadapteroffload.h>
#include <netcx/netexecutioncontext_p.h>
#include <netcx/netmemory_p.h>
