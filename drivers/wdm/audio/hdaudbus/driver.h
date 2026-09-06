#if !defined(_SKLHDAUDBUS_H_)
#define _SKLHDAUDBUS_H_

#define POOL_ZERO_DOWN_LEVEL_SUPPORT

#pragma warning(disable:4200)  // suppress nameless struct/union warning
#pragma warning(disable:4201)  // suppress nameless struct/union warning
#pragma warning(disable:4214)  // suppress bit field types other than int warning
#include <ntddk.h>
#include <initguid.h>

//SGPC (Win10 1809+ support)
extern "C" {
	#define __CPLUSPLUS

	#include <windef.h>
	#include <winerror.h>

	#include <wingdi.h>
	#include <d3dkmddi.h>
	#include <d3dkmthk.h>
}

#include <wdm.h>
#include <wdmguid.h>
#include "wdf.h"
#include <ntintsafe.h>
#include <ntstrsafe.h>
#include <hdaudio.h>
#include <portcls.h>

#include "hda_registers.h"
#include "fdo.h"
#include "buspdo.h"
#include "hdac_controller.h"
#include "hdac_stream.h"
#include "hda_verbs.h"

#ifdef __REACTOS__
#define DRIVERNAME "hdaudbus.sys: "
#else
#define DRIVERNAME "sklhdaudbus.sys: "
#endif
#define SKLHDAUDBUS_POOL_TAG 'SADH'

#define VEN_INTEL 0x8086
#define VEN_ATI 0x1002
#define VEN_AMD 0x1022
#define VEN_NVIDIA 0x10DE
#define VEN_VMWARE 0x15AD

#include "regfuncs.h"

#ifdef __REACTOS__
#define MAXUINT64 ((UINT64)UINT64_MAX)
#define MAXULONG64 ((ULONG64)ULONG64_MAX)
#define MAXULONG32 ((ULONG32)ULONG_MAX)
#endif

NTSTATUS HDA_WaitForTransfer(
	PFDO_CONTEXT fdoCtx,
	UINT16 codecAddr,
	_In_ ULONG Count,
	_Inout_updates_(Count)
	PHDAUDIO_CODEC_TRANSFER CodecTransfer
);
HDAUDIO_BUS_INTERFACE HDA_BusInterface(PVOID Context);
HDAUDIO_BUS_INTERFACE_V2 HDA_BusInterfaceV2(PVOID Context);
HDAUDIO_BUS_INTERFACE_V3 HDA_BusInterfaceV3(PVOID Context);
HDAUDIO_BUS_INTERFACE_BDL HDA_BusInterfaceBDL(PVOID Context);

#define IS_BXT(ven, dev) (ven == VEN_INTEL && dev == 0x5a98)

static inline void mdelay(LONG msec) {
	LARGE_INTEGER Interval;
	Interval.QuadPart = -10 * 1000 * (LONGLONG)msec;
	KeDelayExecutionThread(KernelMode, FALSE, &Interval);
}

static inline void udelay(LONG usec) {
	/*
	 * Microsecond delays have to busy-wait.
	 *
	 * KeDelayExecutionThread cannot resolve finer than one clock tick, which
	 * is 15.625 ms on a PC - so every udelay(10) here actually slept about
	 * fifteen milliseconds, roughly 1500 times longer than it asked for.
	 * ResetHDAController polls up to 1000 times with udelay(10), twice, so a
	 * controller that does not answer cost half a minute in that function
	 * alone instead of the 20 ms intended, and the whole start path took
	 * minutes while everything behind it in PnP waited.
	 *
	 * KeStallExecutionProcessor is the microsecond-resolution one.  It is
	 * chunked because a single stall is meant to be short - the DDK guidance
	 * is 50 us at a time - and this is also called at raised IRQL.
	 */
	while (usec > 50) {
		KeStallExecutionProcessor(50);
		usec -= 50;
	}

	if (usec > 0) {
		KeStallExecutionProcessor(usec);
	}
}

//
// Helper macros
//

#if DBG
#define DEBUG_LEVEL_ERROR   1
#define DEBUG_LEVEL_INFO    2
#define DEBUG_LEVEL_VERBOSE 3

#define DBG_INIT  1
#define DBG_PNP   2
#define DBG_IOCTL 4

#define SklHdAudBusDebugLevel 100
#define SklHdAudBusDebugCategories (DBG_INIT | DBG_PNP | DBG_IOCTL)

#define SklHdAudBusPrint(dbglevel, dbgcategory, fmt, ...) {          \
    if (SklHdAudBusDebugLevel >= dbglevel &&                         \
        (SklHdAudBusDebugCategories & dbgcategory))                 \
		    {                                                           \
        DbgPrint(DRIVERNAME);                                   \
        DbgPrint(fmt, ##__VA_ARGS__);                             \
		    }                                                           \
}
#else
#define SklHdAudBusPrint(dbglevel, fmt, ...) {                       \
}
#endif
#endif
