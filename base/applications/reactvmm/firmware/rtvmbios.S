/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Firmware for a machine that starts the way a PC always has
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * The processor comes out of reset in real mode at the top of the first
 * megabyte, with a code segment whose hidden base does not match its selector.
 * This is what it lands in.
 *
 * What a firmware of this kind owes whatever boots after it is a table of
 * interrupt handlers, a few bytes of well known data at segment forty, and a
 * boot sector in memory with the machine in a state it recognises. That is all
 * this does, and it does it out loud over the serial port, because a machine
 * with no display has to say what it is doing somewhere.
 */

#include <asm.inc>

/* Where the well known data lives, and the few fields anything actually reads */
#define BDA_SEGMENT         HEX(0040)
#define BDA_COM1            HEX(0000)
#define BDA_EQUIPMENT       HEX(0010)
#define BDA_MEMORY_SIZE     HEX(0013)
#define BDA_KEYBOARD_FLAGS  HEX(0017)
#define BDA_KEYBOARD_HEAD   HEX(001A)
#define BDA_KEYBOARD_TAIL   HEX(001C)
#define BDA_DISPLAY_MODE    HEX(0049)
#define BDA_DISPLAY_COLUMNS HEX(004A)
#define BDA_DISPLAY_PAGE    HEX(0062)
#define BDA_TICKS           HEX(006C)

/* The serial port this talks through, and the registers it uses */
#define COM1_BASE           HEX(03F8)
#define UART_DATA           0
#define UART_INTERRUPT      1
#define UART_QUEUE          2
#define UART_LINE_CONTROL   3
#define UART_MODEM_CONTROL  4
#define UART_LINE_STATUS    5

/* Where the manager left a note saying what this machine is */
#define DESCRIPTION_BASE    HEX(0500)
#define DESCRIPTION_MAGIC   HEX(4D565452)
#define DESCRIPTION_MEMORY  8

/* The disk, and the registers this firmware drives it through */
#define ATA_PRIMARY_BASE    HEX(01F0)
#define ATA_DATA            0
#define ATA_SECTOR_COUNT    2
#define ATA_LBA_LOW         3
#define ATA_LBA_MID         4
#define ATA_LBA_HIGH        5
#define ATA_DRIVE           6
#define ATA_STATUS          7
#define ATA_COMMAND         7

/* Where the boot sector is read to, and where this hands control over */
#define BOOT_SEGMENT        HEX(0000)
#define BOOT_OFFSET         HEX(7C00)

/* The two chips, and where this puts their vectors */
#define PIC_MASTER_COMMAND  HEX(0020)
#define PIC_MASTER_DATA     HEX(0021)
#define PIC_SLAVE_COMMAND   HEX(00A0)
#define PIC_SLAVE_DATA      HEX(00A1)
#define PIC_MASTER_VECTOR   HEX(08)
#define PIC_SLAVE_VECTOR    HEX(70)

.code16

/*
 * The image is one segment, and every address in it is an offset from the base
 * the processor was handed. Nothing here is relocated, so this is also what
 * makes the reset vector land where the processor is already looking.
 */
.org 0

/* WRITING A CHARACTER ********************************************************/

/*
 * Puts AL out of the serial port. Everything this firmware says goes through
 * here, including what a guest asks it to print.
 */
SerialWrite:
    push ax
    push dx
    push cx

    mov ah, al

    /*
     * Wait for the sender to be free. It is bounded, so that a port which is
     * not there cannot stop the machine coming up.
     */
    mov cx, HEX(FFFF)
    mov dx, COM1_BASE + UART_LINE_STATUS
SerialWait:
    in al, dx
    test al, HEX(20)
    jnz SerialReady
    loop SerialWait

SerialReady:
    mov dx, COM1_BASE + UART_DATA
    mov al, ah
    out dx, al

    pop cx
    pop dx
    pop ax
    ret

/* Writes the string DS:SI points at, up to its terminator */
SerialWriteString:
    push ax
    push si
SerialNext:
    lodsb
    test al, al
    jz SerialDone
    call SerialWrite
    jmp SerialNext
SerialDone:
    pop si
    pop ax
    ret

/* Writes AL as two hexadecimal digits */
SerialWriteByte:
    push ax
    push bx

    mov bl, al
    shr al, 4
    call SerialWriteDigit
    mov al, bl
    and al, HEX(0F)
    call SerialWriteDigit

    pop bx
    pop ax
    ret

SerialWriteDigit:
    and al, HEX(0F)
    add al, HEX(30)
    cmp al, HEX(39)
    jbe SerialDigitReady
    add al, 7
SerialDigitReady:
    call SerialWrite
    ret

/* THE SERVICES ***************************************************************/

/*
 * Video, of which only the one call that prints a character is answered. A
 * machine with no display still has somewhere to put its output, and anything
 * that boots this way announces itself through this call before it has any
 * idea what the display is.
 */
Int10Handler:
    cmp ah, HEX(0E)
    je Int10Teletype
    cmp ah, HEX(0F)
    je Int10QueryMode
    cmp ah, HEX(00)
    je Int10SetMode
    iret

Int10Teletype:
    push ax
    call SerialWrite
    pop ax
    iret

Int10SetMode:
    /* Taken and ignored, because there is only one thing to be in */
    push ds
    push bx
    mov bx, BDA_SEGMENT
    mov ds, bx
    mov byte ptr ds:[BDA_DISPLAY_MODE], al
    pop bx
    pop ds
    iret

Int10QueryMode:
    /* Eighty columns of the mode that was last set, on page zero */
    mov ah, 80
    mov bh, 0
    push ds
    push bx
    mov bx, BDA_SEGMENT
    mov ds, bx
    mov al, byte ptr ds:[BDA_DISPLAY_MODE]
    pop bx
    pop ds
    iret

/*
 * What the machine has. Nothing but the serial port, which is what the low
 * bits say, and no disk, which is what leaves the drive count at zero.
 */
Int11Handler:
    push ds
    push bx
    mov bx, BDA_SEGMENT
    mov ds, bx
    mov ax, word ptr ds:[BDA_EQUIPMENT]
    pop bx
    pop ds
    iret

/* How much there is below the first megabyte, in kilobytes */
Int12Handler:
    push ds
    push bx
    mov bx, BDA_SEGMENT
    mov ds, bx
    mov ax, word ptr ds:[BDA_MEMORY_SIZE]
    pop bx
    pop ds
    iret

/*
 * Disks. Everything goes to the one drive on the first channel, because that
 * is the only one this firmware knows how to reach.
 *
 * The carry flag is the answer: set means it did not work, and AH says why.
 * That is the whole contract, and getting it backwards is why a loader that
 * reads nothing still believes it has a kernel.
 */
Int13Handler:
    /*
     * The one drive answers to both names. Which name it is called by depends
     * on what was put in it: a boot sector that came off a removable medium
     * was handed a zero and will keep using it, and one off a fixed disk was
     * handed eighty. Refusing either would mean refusing half the things that
     * can boot.
     */
    cmp dl, HEX(80)
    je Int13OurDrive
    cmp dl, HEX(00)
    jne Int13NotOurDrive

Int13OurDrive:

    cmp ah, HEX(00)
    je Int13Reset
    cmp ah, HEX(02)
    je Int13ReadChs
    cmp ah, HEX(08)
    je Int13Parameters
    cmp ah, HEX(15)
    je Int13DiskType
    cmp ah, HEX(41)
    je Int13ExtensionsPresent
    cmp ah, HEX(42)
    je Int13ReadExtended

Int13NotSupported:
    mov ah, HEX(01)
    jmp Int13Failed

Int13NotOurDrive:
    mov ah, HEX(80)
    jmp Int13Failed

Int13Reset:
    call AtaWaitReady
    jc Int13Failed
    xor ah, ah
    jmp Int13Succeeded

/*
 * Reading by cylinder, head and sector, which is how anything old asks. The
 * numbers are handed to the drive exactly as they came in, because the drive
 * is told the same shape this firmware reports.
 */
Int13ReadChs:
    push bx
    push cx
    push dx
    push si
    push di

    /* AL sectors, to ES:BX, from the place CX and DH describe */
    mov byte ptr cs:[AtaSectorCount], al

    mov byte ptr cs:[AtaSector], cl
    and byte ptr cs:[AtaSector], HEX(3F)

    /* The cylinder is ten bits, split across two registers */
    mov al, ch
    mov byte ptr cs:[AtaCylinderLow], al
    mov al, cl
    shr al, 6
    mov byte ptr cs:[AtaCylinderHigh], al

    mov al, dh
    and al, HEX(0F)
    mov byte ptr cs:[AtaHead], al

    /* Addressed the old way, so the mode bit stays down */
    mov byte ptr cs:[AtaUseLba], 0

    call AtaRead

    pop di
    pop si
    pop dx
    pop cx
    pop bx
    jc Int13ReadFailed

    /* AL comes back saying how many were actually moved */
    mov al, byte ptr cs:[AtaSectorCount]
    xor ah, ah
    jmp Int13Succeeded

Int13ReadFailed:
    mov ah, HEX(04)
    jmp Int13Failed

/*
 * The shape of the drive, which a loader uses to work out how to ask for the
 * rest of itself. It has to match what the drive was told, or a read walks off
 * the end of a track and lands somewhere else entirely.
 */
Int13Parameters:
    push ds
    push cs
    pop ds

    mov ax, word ptr ds:[DiskCylinders]
    dec ax

    /* The low eight bits of the cylinder go in CH */
    mov ch, al
    /* Its top two go in the top of CL, with the sector count under them */
    mov cl, ah
    shl cl, 6
    or cl, byte ptr ds:[DiskSectors]

    mov ax, word ptr ds:[DiskHeads]
    dec ax
    mov dh, al

    /* One drive */
    mov dl, 1

    pop ds
    xor ah, ah
    jmp Int13Succeeded

Int13DiskType:
    /* A fixed disk, and how many sectors it has */
    push ds
    push cs
    pop ds
    mov cx, word ptr ds:[DiskSectorsHigh]
    mov dx, word ptr ds:[DiskSectorsLow]
    pop ds
    mov ah, HEX(03)
    jmp Int13Succeeded

/*
 * Whether this firmware can be asked for a sector by its number alone. It can,
 * which is what lets a loader read past where the old way runs out.
 */
Int13ExtensionsPresent:
    cmp bx, HEX(55AA)
    jne Int13NotSupported

    mov bx, HEX(AA55)
    mov ah, HEX(21)
    mov cx, HEX(0001)
    jmp Int13Succeeded

/*
 * Reading by block number, described by a packet the caller points DS:SI at.
 * The packet says how many to move, where to put them, and which one to start
 * from, and the count is written back with how many were really moved.
 */
Int13ReadExtended:
    push bx
    push cx
    push dx
    push si
    push di

    /* How many, from the packet */
    mov al, byte ptr ds:[si + 2]
    mov byte ptr cs:[AtaSectorCount], al

    /* Where they go, as a segment and an offset the packet holds backwards */
    mov bx, word ptr ds:[si + 4]
    mov ax, word ptr ds:[si + 6]
    push es
    mov es, ax

    /* Which one to start at, of which only the low bits can be reached */
    mov ax, word ptr ds:[si + 8]
    mov byte ptr cs:[AtaSector], al
    mov byte ptr cs:[AtaCylinderLow], ah
    mov ax, word ptr ds:[si + 10]
    mov byte ptr cs:[AtaCylinderHigh], al
    mov ax, word ptr ds:[si + 10]
    shr ax, 8
    and al, HEX(0F)
    mov byte ptr cs:[AtaHead], al

    mov byte ptr cs:[AtaUseLba], 1

    call AtaRead

    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    jc Int13ReadFailed

    xor ah, ah
    jmp Int13Succeeded

/*
 * The two ways out. Both have to put the flag back into the caller's own
 * flags, which are on the stack above the return address, because an iret
 * would hand back the ones this handler happens to be holding.
 */
Int13Succeeded:
    push bp
    mov bp, sp
    and word ptr [bp + 6], NOT 1
    pop bp
    retf 2

Int13Failed:
    push bp
    mov bp, sp
    or word ptr [bp + 6], 1
    pop bp
    stc
    retf 2

/*
 * The one that grew into everything else. Only the memory map is answered,
 * which is the one thing a loader genuinely cannot do without.
 */
Int15Handler:
    cmp ax, HEX(E820)
    je Int15MemoryMap
    cmp ah, HEX(88)
    je Int15ExtendedSize

    /* Anything else is refused, and the caller decides what that means */
    stc
    mov ah, HEX(86)
    retf 2

Int15ExtendedSize:
    /* Kilobytes above the first megabyte, which the manager filled in */
    push ds
    push bx
    mov bx, BDA_SEGMENT
    mov ds, bx
    mov ax, word ptr ds:[BDA_MEMORY_SIZE]
    pop bx
    pop ds
    clc
    retf 2

/*
 * The memory map, one range each time it is called. The caller passes back the
 * number this gave it last time, and zero when it has run out.
 */
Int15MemoryMap:
    cmp edx, HEX(534D4150)
    jne Int15MapRefused

    movzx ax, byte ptr cs:[MemoryMapCount]
    cmp bx, ax
    jae Int15MapDone

    /* Twenty bytes, whatever was asked for, because that is what is written */
    push ds
    push si
    push cx

    push cs
    pop ds

    mov si, OFFSET MemoryMap
    mov ax, bx
    mov cx, 20
    mul cx
    add si, ax

    mov cx, 20
    push di
Int15MapCopy:
    lodsb
    stosb
    loop Int15MapCopy
    pop di

    pop cx
    pop si
    pop ds

    mov eax, HEX(534D4150)
    mov ecx, 20
    inc bx
    movzx ax, byte ptr cs:[MemoryMapCount]
    cmp bx, ax
    jb Int15MapMore

    /* The last one is answered with zero, which says not to ask again */
    xor bx, bx
Int15MapMore:
    clc
    retf 2

Int15MapDone:
    xor bx, bx
    clc
    retf 2

Int15MapRefused:
    stc
    mov ah, HEX(86)
    retf 2

/*
 * The keyboard, which is the serial port read the other way. Anything typed at
 * the other end of the pipe arrives here.
 */
Int16Handler:
    cmp ah, HEX(00)
    je Int16Read
    cmp ah, HEX(01)
    je Int16Peek
    cmp ah, HEX(02)
    je Int16Flags
    iret

Int16Read:
    push dx
Int16Wait:
    mov dx, COM1_BASE + UART_LINE_STATUS
    in al, dx
    test al, HEX(01)
    jnz Int16Take

    /* Nothing yet. Let anything else that was waiting have a turn */
    sti
    hlt
    jmp Int16Wait

Int16Take:
    mov dx, COM1_BASE + UART_DATA
    in al, dx
    pop dx

    /* No scan code to give, because nothing here has keys */
    mov ah, 0
    iret

Int16Peek:
    push dx
    mov dx, COM1_BASE + UART_LINE_STATUS
    in al, dx
    pop dx

    test al, HEX(01)
    jz Int16Empty

    /*
     * Something is waiting. It is not read here, because this call is only
     * meant to look, and the read that follows is the one that takes it.
     */
    push dx
    mov dx, COM1_BASE + UART_DATA
    in al, dx
    pop dx
    mov ah, 0

    /* Said to be there, and the flag says so */
    push bp
    mov bp, sp
    and word ptr [bp + 6], NOT HEX(40)
    pop bp
    iret

Int16Empty:
    push bp
    mov bp, sp
    or word ptr [bp + 6], HEX(40)
    pop bp
    iret

Int16Flags:
    xor al, al
    iret

/* The clock, kept by nothing yet, so the count simply stands still */
Int1AHandler:
    cmp ah, HEX(00)
    je Int1AReadTicks
    iret

Int1AReadTicks:
    push ds
    push bx
    mov bx, BDA_SEGMENT
    mov ds, bx
    mov ax, word ptr ds:[BDA_TICKS]
    mov dx, word ptr ds:[BDA_TICKS + 2]
    mov cx, dx
    mov dx, ax
    xor al, al
    pop bx
    pop ds
    iret

/* DRIVING THE DISK ***********************************************************/

/*
 * Waits for the drive to stop being busy and say it is ready.
 *
 * Carry set means it never did. The wait is bounded, because a machine built
 * with no disk still has to finish coming up and say so.
 */
AtaWaitReady:
    push ax
    push cx
    push dx

    mov cx, HEX(FFFF)
    mov dx, ATA_PRIMARY_BASE + ATA_STATUS
AtaWaitBusy:
    in al, dx
    test al, HEX(80)
    jz AtaCheckReady
    loop AtaWaitBusy
    jmp AtaWaitTimedOut

AtaCheckReady:
    test al, HEX(40)
    jz AtaWaitTimedOut

    pop dx
    pop cx
    pop ax
    clc
    ret

AtaWaitTimedOut:
    pop dx
    pop cx
    pop ax
    stc
    ret

/* Waits until the drive has a payload waiting, or says it has failed */
AtaWaitData:
    push ax
    push cx
    push dx

    mov cx, HEX(FFFF)
    mov dx, ATA_PRIMARY_BASE + ATA_STATUS
AtaPoll:
    in al, dx
    test al, HEX(80)
    jnz AtaPollAgain

    /* Refused, which is an answer and not something to keep waiting for */
    test al, HEX(01)
    jnz AtaDataFailed

    test al, HEX(08)
    jnz AtaDataReady

AtaPollAgain:
    loop AtaPoll

AtaDataFailed:
    pop dx
    pop cx
    pop ax
    stc
    ret

AtaDataReady:
    pop dx
    pop cx
    pop ax
    clc
    ret

/*
 * Reads what the registers below describe into ES:BX. Carry says it failed.
 *
 * The sectors come across a word at a time out of the data register, which is
 * what the one instruction that does it is for.
 */
AtaRead:
    push ax
    push cx
    push dx
    push di

    call AtaWaitReady
    jc AtaReadFailed

    /* Which drive, and which way the address is being given */
    mov dx, ATA_PRIMARY_BASE + ATA_DRIVE
    mov al, byte ptr cs:[AtaHead]
    and al, HEX(0F)
    or al, HEX(A0)
    cmp byte ptr cs:[AtaUseLba], 0
    je AtaDriveReady
    or al, HEX(40)
AtaDriveReady:
    out dx, al

    mov dx, ATA_PRIMARY_BASE + ATA_SECTOR_COUNT
    mov al, byte ptr cs:[AtaSectorCount]
    out dx, al

    mov dx, ATA_PRIMARY_BASE + ATA_LBA_LOW
    mov al, byte ptr cs:[AtaSector]
    out dx, al

    mov dx, ATA_PRIMARY_BASE + ATA_LBA_MID
    mov al, byte ptr cs:[AtaCylinderLow]
    out dx, al

    mov dx, ATA_PRIMARY_BASE + ATA_LBA_HIGH
    mov al, byte ptr cs:[AtaCylinderHigh]
    out dx, al

    mov dx, ATA_PRIMARY_BASE + ATA_COMMAND
    mov al, HEX(20)
    out dx, al

    /* One sector at a time, because the drive says ready once for each */
    movzx cx, byte ptr cs:[AtaSectorCount]
    test cx, cx
    jnz AtaReadLoop
    mov cx, 256

AtaReadLoop:
    push cx

    call AtaWaitData
    jc AtaReadPopFailed

    mov di, bx
    mov cx, 256
    mov dx, ATA_PRIMARY_BASE + ATA_DATA
    cld
    rep insw

    /* On to where the next one goes */
    mov bx, di

    pop cx
    loop AtaReadLoop

    pop di
    pop dx
    pop cx
    pop ax
    clc
    ret

AtaReadPopFailed:
    pop cx
AtaReadFailed:
    pop di
    pop dx
    pop cx
    pop ax
    stc
    ret

/*
 * Asks the drive what it is, and takes the shape out of the answer. Without
 * this the firmware would be reporting a shape the drive does not agree with,
 * and every read past the first track would land somewhere else.
 */
AtaIdentify:
    push ax
    push bx
    push cx
    push dx
    push di
    push es

    push cs
    pop es

    call AtaWaitReady
    jc AtaIdentifyFailed

    mov dx, ATA_PRIMARY_BASE + ATA_DRIVE
    mov al, HEX(A0)
    out dx, al

    mov dx, ATA_PRIMARY_BASE + ATA_COMMAND
    mov al, HEX(EC)
    out dx, al

    call AtaWaitData
    jc AtaIdentifyFailed

    mov di, OFFSET IdentifyBuffer
    mov cx, 256
    mov dx, ATA_PRIMARY_BASE + ATA_DATA
    cld
    rep insw

    /* Words one, three and six are the shape it is claiming */
    mov ax, word ptr cs:[IdentifyBuffer + 2]
    mov word ptr cs:[DiskCylinders], ax
    mov ax, word ptr cs:[IdentifyBuffer + 6]
    mov word ptr cs:[DiskHeads], ax
    mov ax, word ptr cs:[IdentifyBuffer + 12]
    mov word ptr cs:[DiskSectors], ax

    /* Words sixty and sixty one are how many there are altogether */
    mov ax, word ptr cs:[IdentifyBuffer + 120]
    mov word ptr cs:[DiskSectorsLow], ax
    mov ax, word ptr cs:[IdentifyBuffer + 122]
    mov word ptr cs:[DiskSectorsHigh], ax

    mov byte ptr cs:[DiskPresent], 1

    /*
     * Eighteen sectors on each of two tracks is the shape of the one removable
     * size that still matters, and nothing else has it. A medium that looks
     * like that is handed over as the first floppy, because that is the name
     * whatever is on it will expect to be given.
     */
    mov byte ptr cs:[BootDrive], HEX(80)
    cmp word ptr cs:[DiskHeads], 2
    jne AtaIdentifyFixed
    cmp word ptr cs:[DiskSectors], 18
    jne AtaIdentifyFixed
    mov byte ptr cs:[BootDrive], HEX(00)

AtaIdentifyFixed:
    pop es
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    clc
    ret

AtaIdentifyFailed:
    mov byte ptr cs:[DiskPresent], 0
    pop es
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    stc
    ret

/*
 * The timer, eighteen or so times a second. It keeps the count anything asking
 * the time of day reads, hands on to whoever wants to know, and tells the
 * controller it is done.
 *
 * That last part is not optional. A controller left holding an interrupt
 * never delivers another one, so a handler that returns without saying so
 * stops the clock for the whole machine.
 */
Int08Handler:
    push ax
    push ds
    push bx

    mov bx, BDA_SEGMENT
    mov ds, bx

    add word ptr ds:[BDA_TICKS], 1
    adc word ptr ds:[BDA_TICKS + 2], 0

    pop bx
    pop ds

    /* Anything that wants to know gets told before this returns */
    int HEX(1C)

    mov al, HEX(20)
    out PIC_MASTER_COMMAND, al

    pop ax
    iret

/*
 * The disk saying it has finished. Nothing waits on it, because every read
 * here is done by asking and waiting rather than by being told, but the
 * controller still has to be answered.
 */
Int76Handler:
    push ax
    mov al, HEX(20)
    out PIC_SLAVE_COMMAND, al
    out PIC_MASTER_COMMAND, al
    pop ax
    iret

/* Anything pointed here is known about and deliberately does nothing */
IgnoreHandler:
    iret

/*
 * Where an interrupt from the controller lands while nothing has claimed it.
 * The controller is told the interrupt is over, because a chip left holding
 * one never delivers another.
 */
SpuriousHandler:
    push ax
    mov al, HEX(20)
    out PIC_MASTER_COMMAND, al
    pop ax
    iret

SpuriousSlaveHandler:
    push ax
    mov al, HEX(20)
    out PIC_SLAVE_COMMAND, al
    out PIC_MASTER_COMMAND, al
    pop ax
    iret

/* COMING UP ******************************************************************/

/*
 * The first thing that runs. Nothing is set up: there is no stack, no table of
 * handlers, and the interrupt controllers are wherever they came up.
 */
Start:
    cli
    cld

    /* A stack, before anything that could need one */
    xor ax, ax
    mov ss, ax
    mov sp, HEX(7000)

    mov ds, ax
    mov es, ax

    call SerialSetup

    push cs
    pop ds
    mov si, OFFSET SignOnMessage
    call SerialWriteString

    call VectorSetup
    call ReadDescription
    call DataAreaSetup
    call ControllerSetup

    /* Ask the drive what it is, so the shape reported later is its own */
    call AtaIdentify
    jc StartNoDisk

    push cs
    pop ds
    mov si, OFFSET DiskFoundMessage
    call SerialWriteString
    jmp StartReady

StartNoDisk:
    push cs
    pop ds
    mov si, OFFSET NoDiskMessage
    call SerialWriteString

StartReady:

    push cs
    pop ds
    mov si, OFFSET ReadyMessage
    call SerialWriteString

    sti

    /* Everything is up, so go and look for something to boot */
    int HEX(19)

    /* Which does not come back */
    jmp HaltForever

/*
 * Reads what the manager wrote about this machine and builds the memory map
 * out of it.
 *
 * Without this the map would describe the first megabyte and nothing else, and
 * anything that believed it would use a megabyte of a machine that has far
 * more. The note is read once and the map is then the firmware's own.
 */
ReadDescription:
    push ax
    push bx
    push cx
    push dx
    push ds
    push es

    xor ax, ax
    mov ds, ax
    push cs
    pop es

    /* Is there a note there at all */
    mov eax, dword ptr ds:[DESCRIPTION_BASE]
    cmp eax, DESCRIPTION_MAGIC
    jne DescriptionMissing

    /* How much memory, of which only the low half is used here */
    mov eax, dword ptr ds:[DESCRIPTION_BASE + DESCRIPTION_MEMORY]
    mov dword ptr es:[MemorySizeLow], eax
    mov eax, dword ptr ds:[DESCRIPTION_BASE + DESCRIPTION_MEMORY + 4]
    mov dword ptr es:[MemorySizeHigh], eax

    /*
     * The range above the first megabyte, which is where everything the
     * machine really has lives. It starts at one megabyte and runs to the end.
     */
    mov eax, dword ptr es:[MemorySizeLow]
    sub eax, HEX(00100000)
    mov dword ptr es:[MemoryMap + 40 + 8], eax
    mov eax, dword ptr es:[MemorySizeHigh]
    sbb eax, 0
    mov dword ptr es:[MemoryMap + 40 + 12], eax

    /* Which makes three ranges rather than two */
    mov byte ptr es:[MemoryMapCount], 3

DescriptionMissing:
    pop es
    pop ds
    pop dx
    pop cx
    pop bx
    pop ax
    ret

/*
 * The port, put into the state everything afterwards assumes: eight bits, no
 * parity, one stop bit, and the divisor for the only rate worth using.
 */
SerialSetup:
    push ax
    push dx

    /* Nothing may interrupt while the port is half configured */
    mov dx, COM1_BASE + UART_INTERRUPT
    xor al, al
    out dx, al

    /* Show the divisor, set it, then put the line control back */
    mov dx, COM1_BASE + UART_LINE_CONTROL
    mov al, HEX(80)
    out dx, al

    mov dx, COM1_BASE + UART_DATA
    mov al, 1
    out dx, al
    mov dx, COM1_BASE + UART_INTERRUPT
    xor al, al
    out dx, al

    mov dx, COM1_BASE + UART_LINE_CONTROL
    mov al, HEX(03)
    out dx, al

    /* Queues on, and both of them emptied */
    mov dx, COM1_BASE + UART_QUEUE
    mov al, HEX(C7)
    out dx, al

    /*
     * Ready, and the line to the controller enabled. That last bit is not a
     * modem signal here: without it the port is wired to nothing.
     */
    mov dx, COM1_BASE + UART_MODEM_CONTROL
    mov al, HEX(0B)
    out dx, al

    pop dx
    pop ax
    ret

/*
 * Fills the table every interrupt goes through. Everything is pointed
 * somewhere that returns, so that an unexpected one is survivable, and then
 * the ones that are really answered are put in over the top.
 */
VectorSetup:
    push ax
    push cx
    push di
    push es

    xor ax, ax
    mov es, ax
    xor di, di

    /* All two hundred and fifty six, pointed at something harmless */
    mov cx, 256
VectorFill:
    mov ax, OFFSET IgnoreHandler
    stosw
    mov ax, cs
    stosw
    loop VectorFill

    mov bx, PIC_MASTER_VECTOR
    mov ax, OFFSET Int08Handler
    call VectorSet

    mov bx, PIC_SLAVE_VECTOR + 6
    mov ax, OFFSET Int76Handler
    call VectorSet

    mov bx, HEX(10)
    mov ax, OFFSET Int10Handler
    call VectorSet

    mov bx, HEX(11)
    mov ax, OFFSET Int11Handler
    call VectorSet

    mov bx, HEX(12)
    mov ax, OFFSET Int12Handler
    call VectorSet

    mov bx, HEX(13)
    mov ax, OFFSET Int13Handler
    call VectorSet

    mov bx, HEX(15)
    mov ax, OFFSET Int15Handler
    call VectorSet

    mov bx, HEX(16)
    mov ax, OFFSET Int16Handler
    call VectorSet

    mov bx, HEX(19)
    mov ax, OFFSET Int19Handler
    call VectorSet

    mov bx, HEX(1A)
    mov ax, OFFSET Int1AHandler
    call VectorSet

    /* Where the controllers are about to be pointed */
    mov bx, PIC_MASTER_VECTOR + 7
    mov ax, OFFSET SpuriousHandler
    call VectorSet

    mov bx, PIC_SLAVE_VECTOR + 7
    mov ax, OFFSET SpuriousSlaveHandler
    call VectorSet

    pop es
    pop di
    pop cx
    pop ax
    ret

/* Puts CS:AX into the entry numbered BX */
VectorSet:
    push es
    push di
    push cx

    push ax
    xor ax, ax
    mov es, ax
    pop ax

    mov di, bx
    shl di, 2
    stosw
    mov ax, cs
    stosw

    pop cx
    pop di
    pop es
    ret

/*
 * The handful of bytes at segment forty that everything reads without asking.
 * The memory sizes are taken from the map the manager wrote, rather than
 * guessed, so that a machine built with a different amount says so.
 */
DataAreaSetup:
    push ax
    push ds

    mov ax, BDA_SEGMENT
    mov ds, ax

    /* Where the first serial port is, which is how anything else finds it */
    mov word ptr ds:[BDA_COM1], COM1_BASE

    /*
     * One serial port, and a drive if one answered. The lowest bit says there
     * is something to boot from at all, which is the first thing anything
     * counting drives looks at.
     */
    mov word ptr ds:[BDA_EQUIPMENT], HEX(0200)
    cmp byte ptr cs:[DiskPresent], 0
    je DataAreaNoDrive
    or word ptr ds:[BDA_EQUIPMENT], HEX(0001)
DataAreaNoDrive:

    /* Six hundred and forty kilobytes, as there always are */
    mov word ptr ds:[BDA_MEMORY_SIZE], 640

    mov byte ptr ds:[BDA_KEYBOARD_FLAGS], 0
    mov word ptr ds:[BDA_KEYBOARD_HEAD], HEX(001E)
    mov word ptr ds:[BDA_KEYBOARD_TAIL], HEX(001E)

    mov byte ptr ds:[BDA_DISPLAY_MODE], 3
    mov word ptr ds:[BDA_DISPLAY_COLUMNS], 80
    mov byte ptr ds:[BDA_DISPLAY_PAGE], 0

    mov word ptr ds:[BDA_TICKS], 0
    mov word ptr ds:[BDA_TICKS + 2], 0

    pop ds
    pop ax
    ret

/*
 * The two controllers, put where a machine of this kind has always had them.
 * They come up answering at the same numbers the processor uses for its own
 * faults, so until this runs an interrupt looks like a divide by zero.
 */
ControllerSetup:
    push ax

    /* Both told to expect the whole sequence that follows */
    mov al, HEX(11)
    out PIC_MASTER_COMMAND, al
    out PIC_SLAVE_COMMAND, al

    /* Where each one's eight land */
    mov al, PIC_MASTER_VECTOR
    out PIC_MASTER_DATA, al
    mov al, PIC_SLAVE_VECTOR
    out PIC_SLAVE_DATA, al

    /* How they are wired to each other, on line two */
    mov al, HEX(04)
    out PIC_MASTER_DATA, al
    mov al, HEX(02)
    out PIC_SLAVE_DATA, al

    /* Plain mode, with the end of an interrupt said rather than assumed */
    mov al, HEX(01)
    out PIC_MASTER_DATA, al
    out PIC_SLAVE_DATA, al

    /*
     * Open the lines something is actually listening for: the timer, the way
     * the second controller arrives, and the serial port. A line nothing
     * answers is worse than no line at all, because the controller is left
     * holding it and never delivers another.
     */
    mov al, HEX(EA)
    out PIC_MASTER_DATA, al

    /* On the second one, the disk */
    mov al, HEX(BF)
    out PIC_SLAVE_DATA, al

    pop ax
    ret

/*
 * Going looking for something to boot: read the first sector of the disk to
 * seven C zero zero and, if it ends the way a boot sector does, jump into it.
 *
 * The two bytes at the end are the only thing that tells a boot sector from
 * an empty disk, so a sector without them is not jumped into. DL is left
 * holding the drive it came from, because that is the one thing the sector is
 * entitled to assume.
 */
Int19Handler:
    sti

    cmp byte ptr cs:[DiskPresent], 0
    je Int19NoDisk

    push cs
    pop ds
    mov si, OFFSET BootingMessage
    call SerialWriteString

    /* The first sector, to where a boot sector has always been put */
    mov ax, BOOT_SEGMENT
    mov es, ax
    mov bx, BOOT_OFFSET

    mov ah, HEX(02)
    mov al, 1
    mov ch, 0
    mov cl, 1
    mov dh, 0
    mov dl, byte ptr cs:[BootDrive]
    int HEX(13)
    jc Int19ReadFailed

    /* Is it one */
    mov ax, BOOT_SEGMENT
    mov es, ax
    cmp word ptr es:[BOOT_OFFSET + 510], HEX(AA55)
    jne Int19NotBootable

    push cs
    pop ds
    mov si, OFFSET HandOverMessage
    call SerialWriteString

    /*
     * Into it. Everything a boot sector is entitled to expect is set: the
     * drive it came from in DL, segments at zero, and a stack under it.
     */
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, BOOT_OFFSET

    mov dl, byte ptr cs:[BootDrive]

    ljmp16 BOOT_SEGMENT, BOOT_OFFSET

Int19ReadFailed:
    push cs
    pop ds
    mov si, OFFSET ReadFailedMessage
    call SerialWriteString
    jmp HaltForever

Int19NotBootable:
    push cs
    pop ds
    mov si, OFFSET NotBootableMessage
    call SerialWriteString
    jmp HaltForever

Int19NoDisk:
    push cs
    pop ds
    mov si, OFFSET NoBootMessage
    call SerialWriteString

HaltForever:
    cli
    hlt
    jmp HaltForever

/* WHAT IT SAYS ***************************************************************/

SignOnMessage:
    .ascii "ReacTVmm firmware"
    .byte 13, 10, 0

ReadyMessage:
    .ascii "vectors, data area and controllers are up"
    .byte 13, 10, 0

NoBootMessage:
    .ascii "no boot device"
    .byte 13, 10, 0

DiskFoundMessage:
    .ascii "disk found"
    .byte 13, 10, 0

NoDiskMessage:
    .ascii "no disk answered"
    .byte 13, 10, 0

BootingMessage:
    .ascii "reading the boot sector"
    .byte 13, 10, 0

HandOverMessage:
    .ascii "handing over"
    .byte 13, 10, 0

ReadFailedMessage:
    .ascii "the boot sector would not read"
    .byte 13, 10, 0

NotBootableMessage:
    .ascii "there is no boot sector on that disk"
    .byte 13, 10, 0

/*
 * What the firmware keeps about the disk, and the registers the reads are
 * built up in. This is in the image because there is nowhere else yet: the
 * data area at segment forty is spoken for and a scratch page would have to
 * be taken from the memory the guest is about to be told it has.
 */
.align 2
DiskPresent:
    .byte 0
BootDrive:
    .byte HEX(80)
AtaUseLba:
    .byte 0
AtaSectorCount:
    .byte 0
AtaSector:
    .byte 0
AtaCylinderLow:
    .byte 0
AtaCylinderHigh:
    .byte 0
AtaHead:
    .byte 0
    .byte 0

DiskCylinders:
    .word 0
DiskHeads:
    .word 0
DiskSectors:
    .word 0
DiskSectorsLow:
    .word 0
DiskSectorsHigh:
    .word 0

.align 2
IdentifyBuffer:
    .fill 512, 1, 0

/*
 * The memory map handed out one range at a time. Two of them: what is below
 * the first megabyte and usable, and the part of it that is not.
 *
 * Each is twenty bytes, being where it starts, how long it is, and what kind
 * it is. One means memory that may be used, two means memory that may not.
 */
.align 4
MemoryMap:
    /* The first six hundred and forty kilobytes */
    .long HEX(00000000), HEX(00000000)
    .long HEX(0009FC00), HEX(00000000)
    .long 1

    /* Everything from there to the top of the first megabyte is spoken for */
    .long HEX(0009FC00), HEX(00000000)
    .long HEX(00060400), HEX(00000000)
    .long 2

    /* And everything above it, whose length the note above filled in */
    .long HEX(00100000), HEX(00000000)
    .long HEX(00000000), HEX(00000000)
    .long 1

/* How many of the three are real, which depends on what the note said */
MemoryMapCount:
    .byte 2
    .byte 0

MemorySizeLow:
    .long 0
MemorySizeHigh:
    .long 0

/* THE RESET VECTOR ***********************************************************/

/*
 * The processor comes out of reset sixteen bytes from the top of the segment,
 * so that is where the only instruction it is guaranteed to see has to be.
 */
.org HEX(FFF0)
ResetVector:
    ljmp16 HEX(F000), Start

    /*
     * The last eleven bytes are a date, a byte saying what kind of machine
     * this is, and a checksum. They are read by very little and cost nothing,
     * and the image has to run to the end of the segment either way: it is
     * placed so that its last byte is the last byte below a megabyte, so an
     * image one short puts the reset vector one byte off and nothing starts.
     */
    .org HEX(FFF5)
    .ascii "01/01/26"
    .byte HEX(00)
    .byte HEX(FC)
    .byte HEX(00)

.endcode16

END
