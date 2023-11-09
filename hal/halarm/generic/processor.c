/*
 * PROJECT:         ReactOS HAL
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            hal/halarm/generic/processor.c
 * PURPOSE:         HAL Processor Routines
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

LONG HalpActiveProcessors;
KAFFINITY HalpDefaultInterruptAffinity;
BOOLEAN HalpProcessorIdentified;
BOOLEAN HalpTestCleanSupported;

/* PRIVATE FUNCTIONS **********************************************************/

VOID
HalpIdentifyProcessor(VOID)
{

}

/* FUNCTIONS ******************************************************************/

#define QEMUUART 0x09000000
volatile unsigned int * UART0DR = (unsigned int *) QEMUUART;

VOID
ARMWriteToUART(UCHAR Data)
{
    *UART0DR = Data;
}

ULONG
DbgPrintEarly(const char *fmt, ...)
{
    va_list args;
    unsigned int i;
    char Buffer[1024];
    PCHAR String = Buffer;

    va_start(args, fmt);
    i = vsprintf(Buffer, fmt, args);
    va_end(args);

    /* Output the message */
    while (*String != 0)
    {
        if (*String == '\n')
        {

            ARMWriteToUART('\r');
        }
        ARMWriteToUART(*String);
        String++;
    }

    return STATUS_SUCCESS;
}



/* Forcefully shove UART data through qemu */
VOID
Rs232PortPutByte(UCHAR ByteToSend)
{
    *UART0DR = ByteToSend;
}


/*
 * @implemented
 */
VOID
NTAPI
HalInitializeProcessor(IN ULONG ProcessorNumber,
                       IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
   DbgPrintEarly("Starting HAL\n");
    /* Do nothing */

    return;
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
HalAllProcessorsStarted(VOID)
{
    /* Do nothing */
    return TRUE;
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
HalStartNextProcessor(IN PLOADER_PARAMETER_BLOCK LoaderBlock,
                      IN PKPROCESSOR_STATE ProcessorState)
{
    /* Ready to start */
    return FALSE;
}

/*
 * @implemented
 */
VOID
NTAPI
HalProcessorIdle(VOID)
{
    /* Enable interrupts and halt the processor */
    _enable();
    UNIMPLEMENTED;
    while (TRUE);
}

/*
 * @implemented
 */
VOID
NTAPI
HalRequestIpi(KAFFINITY TargetProcessors)
{
    /* Not implemented on UP */
    UNIMPLEMENTED;
    while (TRUE);
}
VOID v7_flush_dcache_all(VOID);
/*
 * @implemented
 */
VOID
HalSweepDcache(VOID)
{
    DbgPrintEarly("HalSweepDcache: try dcache sweep");
    /*
     * We get called very early on, before HalInitSystem or any of the Hal*
     * processor routines, so we need to figure out what CPU we're on.
     */
    if (!HalpProcessorIdentified) HalpIdentifyProcessor();

}

/*
 * @implemented
 */
VOID
HalSweepIcache(VOID)
{

    DbgPrintEarly("HAL JUMP");
    /* All ARM cores support the same Icache flush command */
   // KeArmFlushIcache();
}

/* EOF */
