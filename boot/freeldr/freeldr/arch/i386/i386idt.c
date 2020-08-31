
#include <freeldr.h>


KIDTENTRY DECLSPEC_ALIGN(4) i386Idt[64];
KDESCRIPTOR i386IdtDescriptor = {0, 255, (ULONG)i386Idt};

MACH_SET_PIC_IDT_OFFSETS MachSetPicIdtOffsets = NULL;
MACH_HANDLE_IRQ MachHandleIrq = NULL;

/* See entry.S */
extern USHORT i386idtptr;
extern void __cdecl LoadIdt(void);

/* See i386trap.S */
extern void __cdecl i386Irq0(void);
extern void __cdecl i386Irq1(void);
extern void __cdecl i386Irq2(void);
extern void __cdecl i386Irq3(void);
extern void __cdecl i386Irq4(void);
extern void __cdecl i386Irq5(void);
extern void __cdecl i386Irq6(void);
extern void __cdecl i386Irq7(void);
extern void __cdecl i386Irq8(void);
extern void __cdecl i386Irq9(void);
extern void __cdecl i386Irq10(void);
extern void __cdecl i386Irq11(void);
extern void __cdecl i386Irq12(void);
extern void __cdecl i386Irq13(void);
extern void __cdecl i386Irq14(void);
extern void __cdecl i386Irq15(void);

static
void
InitIdtVector(
    UCHAR Vector,
    PVOID ServiceHandler,
    USHORT Access)
{
    i386Idt[Vector].Offset = (ULONG)ServiceHandler & 0xffff;
    i386Idt[Vector].ExtendedOffset = (ULONG)ServiceHandler >> 16;
    i386Idt[Vector].Selector = PMODE_CS;
    i386Idt[Vector].Access = Access;
}

void
__cdecl
InitIdt(void)
{
    InitIdtVector(0, i386DivideByZero, 0x8e00);
    InitIdtVector(1, i386DebugException, 0x8e00);
    InitIdtVector(2, i386NMIException, 0x8e00);
    InitIdtVector(3, i386Breakpoint, 0x8e00);
    InitIdtVector(4, i386Overflow, 0x8e00);
    InitIdtVector(5, i386BoundException, 0x8e00);
    InitIdtVector(6, i386InvalidOpcode, 0x8e00);
    InitIdtVector(7, i386FPUNotAvailable, 0x8e00);
    InitIdtVector(8, i386DoubleFault, 0x8e00);
    InitIdtVector(9, i386CoprocessorSegment, 0x8e00);
    InitIdtVector(10, i386InvalidTSS, 0x8e00);
    InitIdtVector(11, i386SegmentNotPresent, 0x8e00);
    InitIdtVector(12, i386StackException, 0x8e00);
    InitIdtVector(13, i386GeneralProtectionFault, 0x8e00);
    InitIdtVector(14, i386PageFault, 0x8e00);
    InitIdtVector(16, i386CoprocessorError, 0x8e00);
    InitIdtVector(17, i386AlignmentCheck, 0x8e00);
    InitIdtVector(18, i386MachineCheck, 0x8e00);
}

void
i386AcceptIrqs(void)
{
    if (!MachSetPicIdtOffsets)
        return;

    i386IdtDescriptor.Limit = i386idtptr = sizeof(i386Idt) - 1;
    MachSetPicIdtOffsets(32, 40);

    InitIdtVector(32, i386Irq0, 0x8e00);
    InitIdtVector(33, i386Irq1, 0x8e00);
    InitIdtVector(34, i386Irq2, 0x8e00);
    InitIdtVector(35, i386Irq3, 0x8e00);
    InitIdtVector(36, i386Irq4, 0x8e00);
    InitIdtVector(37, i386Irq5, 0x8e00);
    InitIdtVector(38, i386Irq6, 0x8e00);
    InitIdtVector(39, i386Irq7, 0x8e00);

    InitIdtVector(40, i386Irq8, 0x8e00);
    InitIdtVector(41, i386Irq9, 0x8e00);
    InitIdtVector(42, i386Irq10, 0x8e00);
    InitIdtVector(43, i386Irq11, 0x8e00);
    InitIdtVector(44, i386Irq12, 0x8e00);
    InitIdtVector(45, i386Irq13, 0x8e00);
    InitIdtVector(46, i386Irq14, 0x8e00);
    InitIdtVector(47, i386Irq15, 0x8e00);

    LoadIdt();

    /* Enable hardware interrupts */
#if defined(__GNUC__)
    __asm__ __volatile__ ("sti");
#elif defined (_MSC_VER)
    _asm { sti };
#else
#error Cannot decide about inline assembler syntax
#endif
}

void i386Irq0_handler(void)
{
    if (!MachHandleIrq)
        return;
    MachHandleIrq(0);
}

void i386Irq1_handler(void)
{
    if (!MachHandleIrq)
        return;
    MachHandleIrq(1);
}

void i386Irq2_handler(void)
{
    if (!MachHandleIrq)
        return;
    MachHandleIrq(2);
}

void i386Irq3_handler(void)
{
    if (!MachHandleIrq)
        return;
    MachHandleIrq(3);
}

void i386Irq4_handler(void)
{
    if (!MachHandleIrq)
        return;
    MachHandleIrq(4);
}

void i386Irq5_handler(void)
{
    if (!MachHandleIrq)
        return;
    MachHandleIrq(5);
}

void i386Irq6_handler(void)
{
    if (!MachHandleIrq)
        return;
    MachHandleIrq(6);
}

void i386Irq7_handler(void)
{
    if (!MachHandleIrq)
        return;
    MachHandleIrq(7);
}

void i386Irq8_handler(void)
{
    if (!MachHandleIrq)
        return;
    MachHandleIrq(8);
}

void i386Irq9_handler(void)
{
    if (!MachHandleIrq)
        return;
    MachHandleIrq(9);
}

void i386Irq10_handler(void)
{
    if (!MachHandleIrq)
        return;
    MachHandleIrq(10);
}

void i386Irq11_handler(void)
{
    if (!MachHandleIrq)
        return;
    MachHandleIrq(11);
}

void i386Irq12_handler(void)
{
    if (!MachHandleIrq)
        return;
    MachHandleIrq(12);
}

void i386Irq13_handler(void)
{
    if (!MachHandleIrq)
        return;
    MachHandleIrq(13);
}

void i386Irq14_handler(void)
{
    if (!MachHandleIrq)
        return;
    MachHandleIrq(14);
}

void i386Irq15_handler(void)
{
    if (!MachHandleIrq)
        return;
    MachHandleIrq(15);
}
