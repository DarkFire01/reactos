/*
 * PROJECT:     ReactHypervTest
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The machine, as a front end sees it
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * Everything behind this builds a machine out of device libraries and runs a
 * firmware against it. What is in front of it is either a terminal that runs
 * one to the end and says what happened, or a window that shows it running.
 *
 * Neither of those knows anything about the contract the devices speak, and the
 * machine knows nothing about how it is being watched.
 */

#pragma once

#include <windows.h>

#include "vdev.h"

/* As many kinds of device as one machine is worth putting together from */
#define MACHINE_PARTS   8

/* One kind of device, named by the library it is in and what it is called */
struct Part
{
    const char *Library;
    const char *Class;

    /* Whatever that kind takes for itself, or nothing */
    const char *Settings;
};

/* WHAT A MACHINE IS BUILT FROM ***********************************************/

struct VmWanted
{
    const char *Bios;
    Part Parts[MACHINE_PARTS];
    ULONG Many;
    ULONG64 Ram;
};

/* HOW A FRONT END DRIVES ONE *************************************************/

/*
 * Built, run a slice at a time, and taken apart. A front end that has a window
 * to keep answering cannot hand the whole run over and wait for it, so what it
 * gets is as many stops of the processor as it asks for and then control back.
 */
bool VmOpen(_In_ const VmWanted &What);

/* As many stops as asked for, or fewer if it stopped on its own */
ULONG VmRun(_In_ ULONG Steps);

/* Whether it has stopped for good, and where it was when it did */
bool VmStopped();
ULONG64 VmWhere();
ULONG64 VmCount();

/* Back to where a processor is when it comes out of reset, memory and all */
bool VmReset();

void VmClose();

/* What the firmware has said on the first serial port, as far as it has got */
const char *VmSaid();

/* WHAT THERE IS TO LOOK AT ***************************************************/

/*
 * Whichever of the two the display device offers, or neither. A machine with no
 * display in it has nothing to draw, which is not an error: the terminal front
 * end never asks.
 */
IRtvmTextSurface *VmText();
IRtvmPixelSurface *VmPixels();

/* Whether anything has changed since this was last asked, and what */
bool VmDirty(_Out_ VDEV_VIDEO_KIND *Which);

/* HOW THE MACHINE IS PUT TOGETHER ********************************************/

/* Stood in for rather than refused, for a service that is not built yet */
void VmAllowStandIns(_In_ bool Allowed);

/* What the configuration store answers for a slot nobody has named */
void VmStoreAnswers(_In_ HRESULT What);

/* The configuration every device is told, which is XML in UTF-16 */
void VmConfiguration(_In_ const char *Xml);

/* Whether every call into the machine is written down as it happens */
void VmQuiet(_In_ bool Quiet);

/* A run of ports to write down every access to, for watching a disagreement */
void VmWatch(_In_ ULONG First, _In_ ULONG Last);

/* THE TERMINAL FRONT END'S OWN WAY *******************************************/

/* Built, run to the end, taken apart, and an account of it printed */
int Assemble(_In_ const char *Bios,
             _In_reads_(Many) const Part *Parts,
             _In_ ULONG Many,
             _In_ ULONG64 Ram,
             _In_ ULONG Steps);

/* Which is written the way a kind is named, or is not */
bool ReadGuid(_In_ const char *Text, _Out_ GUID &Which);

/* One kind of device, driven on its own with nothing else in the machine */
int One(_In_ const char *Library,
        _In_ const char *Class,
        _In_opt_ const char *Settings);

namespace hv
{

/* A machine with nothing in it at all, for watching a firmware fail on */
int Run(_In_ const char *Path, _In_ ULONG Steps, _In_ ULONG64 Ram);

} /* namespace hv */
