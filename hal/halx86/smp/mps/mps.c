#include <hal.h>
#define NDEBUG
#include <mps.h>
#include <debug.h>
PHYSICAL_ADDRESS HalpLowStubPhysicalAddress;
PVOID HalpLowStub;

VOID
HalpInitializeInterruptLoc(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    /* Do we have a low stub address yet? */
    if (!HalpLowStubPhysicalAddress.QuadPart)
    {
        /* Allocate it */
        HalpLowStubPhysicalAddress.QuadPart = HalpAllocPhysicalMemory(LoaderBlock,
                                                                      0x100000,
                                                                      1,
                                                                      FALSE);
        if (HalpLowStubPhysicalAddress.QuadPart)
        {
            /* Map it */
            HalpLowStub = HalpMapPhysicalMemory64(HalpLowStubPhysicalAddress, 1);
        }
    }

    //HaliFindSmpConfig();
}

VOID
HalpParseApicTables()
{

}