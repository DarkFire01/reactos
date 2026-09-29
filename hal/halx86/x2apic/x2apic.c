/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     x2APIC support in HAL
 * COPYRIGHT:   Copyright 2026 Alex Mendoza <05alex.mendozaa@gmail.com>
 */

/* INCLUDES *******************************************************************/

#include <hal.h>
#include "apic/apicp.h"
#include "x2apicp.h"

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

BOOLEAN HalpX2ApicEnabled = FALSE;

/* Set from the DMAR table, which only the ACPI HALs parse */
BOOLEAN HalpX2ApicFirmwareAllowed = FALSE;

/* FUNCTIONS ******************************************************************/

BOOLEAN
NTAPI
X2ApicIsSupported(VOID)
{
    INT Registers[4];

    __cpuid(Registers, 1);

    return (Registers[2] & (1 << CPUID_X2APIC_FEATURE_BIT)) != 0;
}

/**
 * @brief
 * Decides whether the processors run in x2APIC mode.
 *
 * @param[in] LoaderBlock
 * Loader block carrying the boot options, or NULL.
 *
 * @return
 * TRUE if the boot processor should switch, and every other one after it.
 */
BOOLEAN
NTAPI
X2ApicCheckPolicy(
    _In_opt_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    X2APIC_BASE_ADDRESS_REGISTER BaseRegister;
    PCSTR Options;
    BOOLEAN Allowed;
    INT Registers[4];

    /* Firmware that already switched leaves no choice */
    BaseRegister.LongLong = __readmsr(MSR_APIC_BASE);
    if (BaseRegister.EnableX2Apic)
        return TRUE;

    __cpuid(Registers, 1);
    if (!(Registers[2] & (1 << CPUID_X2APIC_FEATURE_BIT)) ||
        (Registers[2] & (1UL << CPUID_HYPERVISOR_PRESENT_BIT)))
    {
        return FALSE;
    }

    Allowed = HalpX2ApicFirmwareAllowed;

    if (LoaderBlock && LoaderBlock->LoadOptions)
    {
        Options = LoaderBlock->LoadOptions;

        if (strstr(Options, "SAFEBOOT"))
            return FALSE;

        if (strstr(Options, "X2APICPOLICY=ENABLE"))
            Allowed = TRUE;

        if (strstr(Options, "X2APICPOLICY=DISABLE") || strstr(Options, "USELEGACYAPICMODE"))
            Allowed = FALSE;
    }

    return Allowed;
}

/**
 * @brief
 * Puts this processor's local APIC into x2APIC mode.
 *
 * @remarks
 * The mode bit is only accepted while the controller is already on, so both
 * bits are written together. Once set it cannot be cleared without a reset, so
 * a processor that is already in the mode is left alone.
 */
VOID
NTAPI
X2ApicEnable(VOID)
{
    X2APIC_BASE_ADDRESS_REGISTER BaseRegister;

    BaseRegister.LongLong = __readmsr(MSR_APIC_BASE);
    if (BaseRegister.EnableX2Apic)
    {
        HalpX2ApicEnabled = TRUE;
        return;
    }

    BaseRegister.Enable = 1;
    BaseRegister.EnableX2Apic = 1;
    __writemsr(MSR_APIC_BASE, BaseRegister.LongLong);

    HalpX2ApicEnabled = TRUE;
}

/* EOF */
