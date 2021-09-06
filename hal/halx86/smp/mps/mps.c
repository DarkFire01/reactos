
#include <hal.h>
#define NDEBUG
#include <mps.h>
#include <smp.h>
#include <debug.h>
PHYSICAL_ADDRESS HalpLowStubPhysicalAddress;
PVOID HalpLowStub;
#define TEMP_CPUMAX 32
UINT32 PhysicalProcessorCount = 0;
PROCESSOR_IDENTITY HalpStaticProcessorIdentity[TEMP_CPUMAX] = {{0}};
HALP_APIC_INFO_TABLE HalpApicInfoTable;
PPROCESSOR_IDENTITY HalpProcessorIdentity = NULL;

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
HalpParseApicTables(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{

}

VOID
HalpPrintApicTables()
{ 
    DPRINT1("HAL has detected a physical processor count of: %d\n", PhysicalProcessorCount);

    for(UINT32 i = 0; i < PhysicalProcessorCount; i++)
    {
        DPRINT1("Information about the following processor is for processors number: %d\n", i);
        DPRINT1("   The BSPCheck is set to: %X\n", HalpStaticProcessorIdentity[i].BSPCheck);
        DPRINT1("   The LapicID is set to: %X\n", HalpStaticProcessorIdentity[i].LapicId);
        DPRINT1("   The ProcessorID is set to: %X\n", HalpStaticProcessorIdentity[i].ProcessorId);
        DPRINT1("   The ProcessorStated check is set to: %X\n", HalpStaticProcessorIdentity[i].ProcessorStarted);
        DPRINT1("   The ROSProcessorNumber is set to: %X\n", HalpStaticProcessorIdentity[i].ROSProcessorNumber);
    }
}