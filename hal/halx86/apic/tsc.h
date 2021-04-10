/*
 * PROJECT:         ReactOS HAL
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            hal/halx86/apic/tsc.h
 * PURPOSE:         HAL Routines for TSC handling
 * PROGRAMMERS:     Timo Kreuzer (timo.kreuzer@reactos.org)
 */

#ifndef _TSC_H_
#define _TSC_H_

#define NUM_SAMPLES 4
#define MSR_RDTSC 0x10

#ifndef __ASM__

void __cdecl TscCalibrationISR(void);
extern LARGE_INTEGER HalpCpuClockFrequency;
VOID NTAPI HalpInitializeTsc(void);

#ifdef _M_AMD64
#define KiGetIdtEntry(Pcr, Vector) &((Pcr)->IdtBase[Vector])
#else
#define KiGetIdtEntry(Pcr, Vector) &((Pcr)->IDT[Vector])
#endif

#endif

#endif /* !_TSC_H_ */
