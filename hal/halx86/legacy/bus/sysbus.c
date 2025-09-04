/*
 * PROJECT:         ReactOS HAL
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            hal/halx86/legacy/bus/sysbus.c
 * PURPOSE:
 * PROGRAMMERS:     Stefan Ginsberg (stefan.ginsberg@reactos.org)
 */

/* INCLUDES *******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* PRIVATE FUNCTIONS **********************************************************/

BOOLEAN
NTAPI
HalpTranslateSystemBusAddress(IN PBUS_HANDLER BusHandler,
                              IN PBUS_HANDLER RootHandler,
                              IN PHYSICAL_ADDRESS BusAddress,
                              IN OUT PULONG AddressSpace,
                              OUT PPHYSICAL_ADDRESS TranslatedAddress)
{
    PSUPPORTED_RANGE Range = NULL;
    ULONGLONG Addr = BusAddress.QuadPart;

    /* Check what kind of address space this is */
    switch (*AddressSpace)
    {
        /* Memory address */
        case 0:

            /* Loop all prefetch memory */
            for (Range = &BusHandler->BusAddresses->PrefetchMemory;
                 Range;
                 Range = Range->Next)
            {
                /* Check if it's in a valid range */
                if ((BusAddress.QuadPart >= Range->Base) &&
                    (BusAddress.QuadPart <= Range->Limit))
                {
                    /* Get out */
                    break;
                }
            }

            /* Check if we haven't found anything yet */
            if (!Range)
            {
                /* Loop all bus memory */
                for (Range = &BusHandler->BusAddresses->Memory;
                     Range;
                     Range = Range->Next)
                {
                    /* Check if it's in a valid range */
                    if ((BusAddress.QuadPart >= Range->Base) &&
                        (BusAddress.QuadPart <= Range->Limit))
                    {
                        /* Get out */
                        break;
                    }
                }
            }

            /* Done */
            break;

        /* I/O Space */
        case 1:

            /* Loop all bus I/O memory */
            for (Range = &BusHandler->BusAddresses->IO;
                 Range;
                 Range = Range->Next)
            {
                /* Check if it's in a valid range */
                if ((BusAddress.QuadPart >= Range->Base) &&
                    (BusAddress.QuadPart <= Range->Limit))
                {
                    /* Get out */
                    break;
                }
            }

            /* Done */
            break;
    }

    /* Check if we found a range */
    if (Range)
    {
        /* Do the translation and return the kind of address space this is */
        TranslatedAddress->QuadPart = Addr + Range->SystemBase;
        if ((TranslatedAddress->QuadPart != Addr) ||
            (*AddressSpace != Range->SystemAddressSpace))
        {
            /* Informational only; don't spam high-importance channel */
            DPRINT("Translation of %I64x -> %I64x (%s)\n",
                   Addr,
                   TranslatedAddress->QuadPart,
                   Range->SystemAddressSpace ? "I/O" : "MEM");
        }
        *AddressSpace = Range->SystemAddressSpace;
        return TRUE;
    }

    /* Suppress noisy failures for known probe/sizing sentinel values often used by PCI/legacy code.
       These values are not expected to translate on the root/ISA buses and failure is normal: */
    if ((Addr >= 0xFFF00000ULL) ||      /* High sentinel range */
        (Addr == ~0ULL) ||              /* 0xFFFFFFFFFFFFFFFF full mask */
        (Addr == 0xFFFFFFFFULL) ||      /* 32-bit full mask */
        (Addr == 0xFFFFFFFEULL))        /* Common BAR size probe */
    {
        return FALSE;
    }

    /* Nothing found – keep a warning but at reduced severity */
    DPRINT("Hal: SystemBus translation failed for %I64x (space %lu)\n", Addr, *AddressSpace);
    return FALSE;
}

ULONG
NTAPI
HalpGetSystemInterruptVector(IN PBUS_HANDLER BusHandler,
                             IN PBUS_HANDLER RootHandler,
                             IN ULONG BusInterruptLevel,
                             IN ULONG BusInterruptVector,
                             OUT PKIRQL Irql,
                             OUT PKAFFINITY Affinity)
{
    ULONG Vector;
    static BOOLEAN WarnedReuse[MAXIMUM_IDTVECTOR+1];

    /* Get the root vector */
    Vector = HalpGetRootInterruptVector(BusInterruptLevel,
                                        BusInterruptVector,
                                        Irql,
                                        Affinity);

    /* Check if the vector is owned by the HAL and fail if it is */
    if (HalpIDTUsageFlags[Vector].Flags & IDT_REGISTERED)
    {
        /* Shared line (e.g. multiple ISA devices on same IRQ). Return vector instead of 0.
           Warn only once per vector to avoid log spam. */
        if (!WarnedReuse[Vector])
        {
            DPRINT("Vector %lx already registered – allowing shared use.\n", Vector);
            WarnedReuse[Vector] = TRUE;
        }
    }
    return Vector;
}

/* EOF */
