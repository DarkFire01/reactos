#include <hal.h>

VOID
NTAPI
HalReturnToFirmware(
    _In_ FIRMWARE_REENTRY Action)
{
    /* Check what kind of action this is */
    switch (Action)
    {
        /* All recognized actions */
        case HalHaltRoutine:
        case HalPowerDownRoutine:
        case HalRestartRoutine:
        case HalRebootRoutine:
        {
            /* Acquire the display */
            InbvAcquireDisplayOwnership();

            /* Call the internal reboot function */
            //HalpReboot();
        }

        /* Anything else */
        default:
        {
            DbgBreakPoint();
        }
    }
}

