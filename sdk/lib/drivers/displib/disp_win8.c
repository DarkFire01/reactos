/*
 * PROJECT:     ReactOS Display Driver Model
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DISPLIB static library windows 8+
 * COPYRIGHT:   Copyright 2023 Justin Miller <justinmiller100@gmail.com>
 */

#include "pdisplib.h"
//#define NDEBUG
#include <debug.h>

/*
 * Some information:
 * First off, unless this library is totally implemented 100% stable, WDDM drivers
 * compiled with the ReactOS toolchain will never start. This is because
 * as far as i can tell the managment of DXGKRNL is mostly done within this library
 * Starting the driver, prepping function call backs to pass to dxgkrnl are all done here.
 *
 * this also means that DXGKRNL as a service doesn't start unless a driver has invoked it
 * as of this recent commit this behavior is now true on ReactOS as well :).
 */
NTSTATUS
NTAPI
DxgkInitializeDisplayOnlyDriver(_In_ PDRIVER_OBJECT              DriverObject,
                                _In_ PUNICODE_STRING             RegistryPath,
                                _In_ PKMDDOD_INITIALIZATION_DATA KmdDodInitializationData)
{
    NTSTATUS Status;
    PDXGKPORT_INITIALIZE DxgkPortKmdodInitialize;

    DPRINT("Displib: DxgkInitialize entry - Starting a WDDM Driver\n");
    if (!DriverObject,
        !RegistryPath,
        !KmdDodInitializationData)
    {
        DPRINT("DriverObject, KmdDodInitializationData or RegistryPath is NULL\n");
        return STATUS_INVALID_PARAMETER;
    }

    /* It appears Windows will actually fail if the Miniport is below a specific version - we don't care - but we need to know */
    DPRINT("Displib: This WDDM Miniport target version is %X", KmdDodInitializationData->Version);
    Status = DisplibLoadDxgkrnl(IOCTL_VIDEO_KMDOD_DDI_REGISTER,
                                &DxgkPortKmdodInitialize);
    if (Status != STATUS_SUCCESS)
    {
        DPRINT1("Displib has failed to load the DxgkPortKmdodInitialize Callback with status: %x\n", Status);
        return Status;
    }
    DPRINT("Displib: Custom WDDM Driver has passed - IOCTL_VIDEO_DDI_FUNC_REGISTER sent\n");
    Status = DxgkPortKmdodInitialize(DriverObject, RegistryPath, KmdDodInitializationData);
    DPRINT("Displib: return from MiniportInitialization Success - started the WDDM\n");
    return Status;
}
