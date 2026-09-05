
#pragma once

/*
 * The band of interrupt vectors that belongs to device interrupts on the APIC
 * HALs. The ACPI root device claims it so the ACPI driver can hand the vectors
 * out; the vectors below and above it belong to the HAL and to the kernel.
 *
 * It is one vector per I/O APIC input, and the APIC HAL relies on that: with
 * no interrupt owner loaded it translates an input to a vector by biasing the
 * input with HALP_DEVICE_VECTOR_FIRST, the same way the 8259 HAL biases an IRQ
 * with PRIMARY_VECTOR_BASE.
 */
#define HALP_DEVICE_VECTOR_FIRST    0x51
#define HALP_DEVICE_VECTOR_COUNT    110

#ifdef _MINIHAL_
#define VECTOR2IRQ(vector)	((vector) - PRIMARY_VECTOR_BASE)
#define VECTOR2IRQL(vector)	(PROFILE_LEVEL - VECTOR2IRQ(vector))
#define IRQ2VECTOR(irq)		((irq) + PRIMARY_VECTOR_BASE)
#define HalpVectorToIrq(vector)	((vector) - PRIMARY_VECTOR_BASE)
#define HalpVectorToIrql(vector)	(PROFILE_LEVEL - VECTOR2IRQ(vector))
#define HalpIrqToVector(irq)		((irq) + PRIMARY_VECTOR_BASE)
#else

UCHAR
FASTCALL
HalpIrqToVector(UCHAR Irq);

KIRQL
FASTCALL
HalpVectorToIrql(UCHAR Vector);

UCHAR
FASTCALL
HalpVectorToIrq(UCHAR Vector);

#define VECTOR2IRQ(vector)	HalpVectorToIrq(vector)
#define VECTOR2IRQL(vector)	HalpVectorToIrql(vector)
#define IRQ2VECTOR(irq)		HalpIrqToVector(irq)

#endif

