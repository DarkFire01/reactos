
#include "k32_vista.h"

/*
 * @implemented
 */
ULONGLONG
WINAPI
GetTickCount64(VOID)
{
    ULARGE_INTEGER TickCount;

    while (TRUE)
    {
        TickCount.HighPart = (ULONG)SharedUserData->TickCount.High1Time;
        TickCount.LowPart = SharedUserData->TickCount.LowPart;

        if (TickCount.HighPart == (ULONG)SharedUserData->TickCount.High2Time) break;

        YieldProcessor();
     }

     return (UInt32x32To64(TickCount.LowPart, SharedUserData->TickCountMultiplier) >> 24) +
            (UInt32x32To64(TickCount.HighPart, SharedUserData->TickCountMultiplier) << 8);
}

/*
 * @implemented
 */
DWORD
WINAPI
GetTickCount(VOID)
{
    ULARGE_INTEGER TickCount;

#ifdef _WIN64
    TickCount.QuadPart = *((volatile ULONG64*)&SharedUserData->TickCount);
#else
    while (TRUE)
    {
        TickCount.HighPart = (ULONG)SharedUserData->TickCount.High1Time;
        TickCount.LowPart = SharedUserData->TickCount.LowPart;

        if (TickCount.HighPart == (ULONG)SharedUserData->TickCount.High2Time)
            break;

        YieldProcessor();
    }
#endif

    return (ULONG)((UInt32x32To64(TickCount.LowPart,
                                  SharedUserData->TickCountMultiplier) >> 24) +
                    UInt32x32To64((TickCount.HighPart << 8) & 0xFFFFFFFF,
                                  SharedUserData->TickCountMultiplier));

}


/******************************************************************************
 *           QueryInterruptTime  (kernelbase.@)
 */
 void WINAPI DECLSPEC_HOTPATCH QueryInterruptTime( ULONGLONG *time )
 {
     ULONG high, low;
 
     do
     {
         high = SharedUserData->InterruptTime.High1Time;
         low = SharedUserData->InterruptTime.LowPart;
     }
     while (high != SharedUserData->InterruptTime.High2Time);
     *time = (ULONGLONG)high << 32 | low;
 }