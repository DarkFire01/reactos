/* INCLUDES ******************************************************************/

#include <hal.h>
#include "efi.h"
#define NDEBUG
#include <debug.h>

/* FUNCTIONS *****************************************************************/

BOOLEAN
NTAPI
HalQueryRealTimeClock(OUT PTIME_FIELDS Time)
{
    /* First let's verify that the EFI runtime service exists */
    if (0)
    {
        /* Set the time data */
        Time->Second =            0;
        Time->Minute =            0;
        Time->Hour =              0;
        Time->Weekday =           0;
        Time->Day =               0;
        Time->Month =             0;
        Time->Year =              0;
        Time->Milliseconds =      0;
        //("UEFI: HalQueryRealTimeClock Is unimplemented.");
    }

    /* EFI service hasn't succeeded for whatever reason, fallback to architecture */
    HalArchQueryRealTimeClock(Time);

    /* Always return TRUE */
    return TRUE;
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
HalSetRealTimeClock(IN PTIME_FIELDS Time)
{
    /* First let's verify that the EFI runtime service exists */
    if (0)
    {
        //("UEFI: HalSetRealTimeClock Is unimplemented.");
    }

    /* EFI service hasn't succeeded for whatever reason, fallback to architecture */
    HalArchSetRealTimeClock(Time);

    /* Always return TRUE */
    return TRUE;
}
