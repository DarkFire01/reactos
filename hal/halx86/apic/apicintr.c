/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     APIC Interrupt Management Functions
 * COPYRIGHT:   Copyright 2025 ReactOS Development Team
 */

/* INCLUDES *******************************************************************/

#include <hal.h>
#include "apicp.h"
#include <smp.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

extern BOOLEAN HalpApicInitialized;

/* FUNCTIONS ******************************************************************/

/*
 * @brief Gets the APIC interrupt vector for a given IRQ
 * @param Context - Unused context parameter
 * @param Irq - IRQ number to translate
 * @param Vector - Receives the APIC vector
 * @return NTSTATUS - Success or error code
 */
NTSTATUS
NTAPI
HalpApicGetInterruptVector(IN PVOID Context,
                          IN ULONG Irq,
                          OUT PULONG Vector)
{
    UNREFERENCED_PARAMETER(Context);
    
    /* Validate parameters */
    if (!Vector)
        return STATUS_INVALID_PARAMETER;
        
    /* Check if APIC is initialized */
    if (!HalpApicInitialized)
        return STATUS_NOT_SUPPORTED;
        
    /* Validate IRQ range */
    if (Irq > 255)
        return STATUS_INVALID_PARAMETER;
        
    /* Get the vector for this IRQ */
    *Vector = HalpIrqToVector((UCHAR)Irq);
    
    DPRINT("HalpApicGetInterruptVector: IRQ %d -> Vector 0x%x\n", Irq, *Vector);
    
    return STATUS_SUCCESS;
}

/*
 * @brief Enables or disables an APIC interrupt vector
 * @param Context - Unused context parameter
 * @param Vector - APIC vector to enable/disable
 * @param Enable - TRUE to enable, FALSE to disable
 * @return NTSTATUS - Success or error code
 */
NTSTATUS
NTAPI
HalpApicSetInterruptState(IN PVOID Context,
                         IN ULONG Vector,
                         IN BOOLEAN Enable)
{
    UNREFERENCED_PARAMETER(Context);
    
    /* Check if APIC is initialized */
    if (!HalpApicInitialized)
        return STATUS_NOT_SUPPORTED;
        
    /* Validate vector range */
    if (Vector < 0x30 || Vector > 0xFF)
        return STATUS_INVALID_PARAMETER;
        
    if (Enable)
    {
        /* Enable the interrupt */
        HalEnableSystemInterrupt(Vector, PASSIVE_LEVEL, LevelSensitive);
        DPRINT("HalpApicSetInterruptState: Enabled vector 0x%x\n", Vector);
    }
    else
    {
        /* Disable the interrupt */
        HalDisableSystemInterrupt(Vector, PASSIVE_LEVEL);
        DPRINT("HalpApicSetInterruptState: Disabled vector 0x%x\n", Vector);
    }
    
    return STATUS_SUCCESS;
}

/*
 * @brief Enhanced APIC-aware interrupt translation for PCI devices
 * @param Source - Source interrupt descriptor
 * @param Target - Target interrupt descriptor
 * @param Direction - Translation direction
 * @return NTSTATUS - Success or error code
 */
NTSTATUS
NTAPI
HalpApicTranslateInterrupt(IN PCM_PARTIAL_RESOURCE_DESCRIPTOR Source,
                          OUT PCM_PARTIAL_RESOURCE_DESCRIPTOR Target,
                          IN RESOURCE_TRANSLATION_DIRECTION Direction)
{
    /* Validate parameters */
    if (!Source || !Target || Source->Type != CmResourceTypeInterrupt)
        return STATUS_INVALID_PARAMETER;
        
    /* Check if APIC is initialized */
    if (!HalpApicInitialized)
    {
        /* Fallback to simple copy */
        *Target = *Source;
        return STATUS_SUCCESS;
    }
    
    /* Copy source to target as base */
    *Target = *Source;
    
    /* Enhanced APIC interrupt translation */
    if (Direction == TranslateChildToParent)
    {
        /* Child to parent: PCI device IRQ to APIC vector */
        ULONG Vector = Source->u.Interrupt.Vector;
        
        /* For APIC systems, translate PCI IRQ line to APIC vector */
        if (Vector <= 15)
        {
            /* Legacy IRQ range: map to standard APIC vectors */
            Target->u.Interrupt.Vector = 0x30 + Vector;
            Target->u.Interrupt.Level = 0x30 + Vector;
            
            /* APIC interrupts are edge-triggered by default for PCI */
            Target->Flags = CM_RESOURCE_INTERRUPT_LATCHED;
            
            DPRINT("HalpApicTranslateInterrupt: Child->Parent, IRQ %d -> Vector 0x%x\n", 
                   Vector, Target->u.Interrupt.Vector);
        }
        else if (Vector >= 16 && Vector <= 23)
        {
            /* APIC extended IRQ range for PCI devices */
            Target->u.Interrupt.Vector = 0x40 + (Vector - 16);
            Target->u.Interrupt.Level = 0x40 + (Vector - 16);
            Target->Flags = CM_RESOURCE_INTERRUPT_LATCHED;
            
            DPRINT("HalpApicTranslateInterrupt: Child->Parent, Extended IRQ %d -> Vector 0x%x\n", 
                   Vector, Target->u.Interrupt.Vector);
        }
        else
        {
            /* High vectors, keep as-is */
            DPRINT("HalpApicTranslateInterrupt: Child->Parent, Vector 0x%x (no translation)\n", Vector);
        }
    }
    else
    {
        /* Parent to child: APIC vector to PCI device IRQ */
        ULONG Vector = Source->u.Interrupt.Vector;
        
        if (Vector >= 0x30 && Vector <= 0x3F)
        {
            /* Legacy IRQ range: map from APIC vector back to IRQ */
            Target->u.Interrupt.Vector = Vector - 0x30;
            Target->u.Interrupt.Level = Vector - 0x30;
            
            DPRINT("HalpApicTranslateInterrupt: Parent->Child, Vector 0x%x -> IRQ %d\n", 
                   Vector, Target->u.Interrupt.Vector);
        }
        else if (Vector >= 0x40 && Vector <= 0x47)
        {
            /* Extended IRQ range: map from APIC vector back to IRQ */
            Target->u.Interrupt.Vector = 16 + (Vector - 0x40);
            Target->u.Interrupt.Level = 16 + (Vector - 0x40);
            
            DPRINT("HalpApicTranslateInterrupt: Parent->Child, Vector 0x%x -> Extended IRQ %d\n", 
                   Vector, Target->u.Interrupt.Vector);
        }
        else
        {
            DPRINT("HalpApicTranslateInterrupt: Parent->Child, Vector 0x%x (no translation)\n", Vector);
        }
    }
    
    return STATUS_SUCCESS;
}
