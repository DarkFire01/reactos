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
#define BDA_DISPLAY_ROWS    HEX(0084)

/* The clock, and the registers of it that say what the time and date are */
#define CMOS_ADDRESS        HEX(70)
#define CMOS_DATA           HEX(71)
#define CMOS_SECONDS        HEX(00)
#define CMOS_MINUTES        HEX(02)
#define CMOS_HOURS          HEX(04)
#define CMOS_DAY            HEX(07)
#define CMOS_MONTH          HEX(08)
#define CMOS_YEAR           HEX(09)
#define CMOS_CENTURY        HEX(32)
#define BDA_TICKS           HEX(006C)

/* The serial port this talks through, and the registers it uses */
#define COM1_BASE           HEX(03F8)
#define UART_DATA           0
#define UART_INTERRUPT      1
#define UART_QUEUE          2
#define UART_LINE_CONTROL   3
#define UART_MODEM_CONTROL  4
#define UART_LINE_STATUS    5
#define UART_SCRATCH        7

/* The display, its page and the registers that say where the cursor is */
#define VIDEO_SEGMENT       HEX(0B800)
#define VIDEO_COLUMNS       80
#define VIDEO_ROWS          25
#define VIDEO_CRTC_ADDRESS  HEX(03D4)

/* Where the cursor for the first page is kept */
#define BDA_CURSOR          HEX(0050)
/* Ours, in the part of the area nothing else was given */
#define BDA_FLOPPY_DONE     HEX(003E)

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

/* The two ports everything on the bus is asked about through */
#define PCI_ADDRESS_PORT    HEX(0CF8)
#define PCI_DATA_PORT       HEX(0CFC)

/* Where in a device's description each of the things this reads is */
#define PCI_WHO             HEX(00)
#define PCI_DOING           HEX(04)
#define PCI_WHAT            HEX(08)
#define PCI_LAYOUT          HEX(0C)
#define PCI_FIRST_RANGE     HEX(10)
#define PCI_PAST_RANGES     HEX(28)

/* What a place with nothing in it says when it is asked who is there */
#define PCI_NOBODY          HEX(FFFF)

/* That a place has more to it than its first part */
#define PCI_HAS_MORE        HEX(80)

/* The two kinds of address a device is told it may answer at */
#define PCI_DECODE          HEX(03)

/* And where this starts handing each of them out */
#define PCI_FIRST_PORT      HEX(C000)
#define PCI_FIRST_MEMORY    HEX(E0000000)

/*
 * The disk controller, and the pair of registers that says it may answer at
 * the addresses it has always had. Nothing on the bus says where those are:
 * a controller in this mode is found by asking and then driven at numbers
 * everything already knows, and this bit is what connects the two.
 */
#define PCI_IS_DISK         HEX(0101)
#define PCI_DISK_TIMING     HEX(40)
#define PCI_DISK_DECODES    HEX(80008000)

/*
 * The shape of the removable drive a disc pretends to be, by which kind it
 * named, and how many of its sectors fit in one of the disc.
 */
#define CD_PRETEND_NONE     0
#define CD_PRETEND_SMALL    1
#define CD_PRETEND_USUAL    2
#define CD_PRETEND_LARGE    3

#define CD_SMALL_SECTORS    15
#define CD_USUAL_SECTORS    18
#define CD_LARGE_SECTORS    36
#define CD_PRETEND_TRACKS   80
#define CD_PRETEND_SIDES    2

#define CD_PRETEND_SECTOR   512
#define CD_PER_DISC_SECTOR  4

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

/* WHETHER THERE IS A PORT THERE AT ALL ***************************************/

/*
 * A port nobody answers for reads back with every bit set, and every bit set is
 * what a port says when it has a character waiting. So a firmware that believes
 * the line status of a port that is not there believes a character has arrived,
 * over and over, forever: anything asking it for a key is handed one that was
 * never typed and never sees the keys that were.
 *
 * So it is asked once whether it is there. The last of its registers is the one
 * the chip keeps nothing of its own in, which makes it the one that can be
 * written and read back, and a port that does not hold what was put in it is
 * not a port.
 */
Com1There:
    .byte 0

SerialProbe:
    push ax
    push dx

    mov byte ptr cs:[Com1There], 0

    mov dx, COM1_BASE + UART_SCRATCH
    mov al, HEX(5A)
    out dx, al
    in al, dx
    cmp al, HEX(5A)
    jne SerialProbeDone

    /* And again with the other half of the byte, so that a bus which happens
       to be holding the first pattern is not mistaken for a chip */
    mov al, HEX(A5)
    out dx, al
    in al, dx
    cmp al, HEX(A5)
    jne SerialProbeDone

    mov byte ptr cs:[Com1There], 1

SerialProbeDone:
    pop dx
    pop ax
    ret

/* Writes the string DS:SI points at, up to its terminator */
/*
 * Puts the number in EAX out of the serial port, as eight characters with the
 * top one first. For seeing what something asked this firmware to do, which is
 * the only way to tell a caller that is getting on with it from one that is
 * asking for the same thing over and over.
 */
SerialWriteHex:
    push eax
    push ebx
    push cx

    mov ebx, eax
    mov cx, 8

SerialWriteHexNext:
    rol ebx, 4
    mov al, bl
    and al, HEX(0F)
    add al, HEX(30)
    cmp al, HEX(39)
    jbe SerialWriteHexDigit
    add al, 7

SerialWriteHexDigit:
    call SerialWrite
    dec cx
    jnz SerialWriteHexNext

    pop cx
    pop ebx
    pop eax
    ret

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

/* THE SERVICES ***************************************************************/

/*
 * The video calls, answered against the text page as well as the serial port.
 *
 * Everything an operating system prints goes through here. The teletype call
 * alone is enough for a banner, because a banner is only ever appended; a
 * prompt is not, and neither is anything that draws. Those need the cursor to
 * be somewhere, to be readable, and to move where it is put, which is what the
 * rest of these do.
 *
 * Every character goes to the serial port too, so that a machine with nothing
 * looking at its screen can still be watched.
 */
Int10Handler:
    cmp ah, HEX(00)
    je Int10SetMode
    cmp ah, HEX(01)
    je Int10SetShape
    cmp ah, HEX(02)
    je Int10SetCursor
    cmp ah, HEX(03)
    je Int10GetCursor
    cmp ah, HEX(05)
    je Int10SetPage
    cmp ah, HEX(06)
    je Int10ScrollUp
    cmp ah, HEX(07)
    je Int10ScrollDown
    cmp ah, HEX(08)
    je Int10ReadCell
    cmp ah, HEX(09)
    je Int10WriteCell
    cmp ah, HEX(0A)
    je Int10WriteCell
    cmp ah, HEX(0E)
    je Int10Teletype
    cmp ah, HEX(0F)
    je Int10QueryMode
    cmp ah, HEX(12)
    je Int10Subsystem
    cmp ah, HEX(13)
    je Int10WriteString
    cmp ah, HEX(4F)
    je Int10Vbe
    cmp ah, HEX(1A)
    je Int10DisplayCode
    iret

Int10SetMode:
    /*
     * The display is actually put into the mode, not merely told which one was
     * asked for. Anything that asks for a mode and then writes pixels is
     * entitled to find the display arranged for pixels; one still arranged for
     * characters throws every one of those writes away and shows nothing.
     *
     * The top bit says not to clear what is there, which is the one thing
     * about this call that has nothing to do with the mode.
     */
    push ax
    and al, HEX(7F)
    call VgaSetMode
    pop ax

    push ds
    push bx
    mov bx, BDA_SEGMENT
    mov ds, bx
    mov byte ptr ds:[BDA_DISPLAY_MODE], al
    mov word ptr ds:[BDA_CURSOR], 0
    pop bx
    pop ds

    test al, HEX(80)
    jnz Int10ModeDone

    /* A mode made of pixels has nothing to clear that characters would */
    push ax
    and al, HEX(7F)
    cmp al, HEX(12)
    je Int10ClearPixels
    call VideoClear
    pop ax
    iret

Int10ClearPixels:
    call VgaClearPlanes
    pop ax

Int10ModeDone:
    iret

Int10SetShape:
Int10SetPage:
Int10Subsystem:
    /* Nothing to set, and saying so plainly is better than not answering */
    iret

Int10DisplayCode:
    /* A colour display of the usual kind, which is the only answer worth giving */
    mov al, HEX(1A)
    mov bl, HEX(08)
    mov bh, HEX(00)
    iret

Int10SetCursor:
    push ds
    push bx
    mov bx, BDA_SEGMENT
    mov ds, bx
    mov byte ptr ds:[BDA_CURSOR], dl
    mov byte ptr ds:[BDA_CURSOR + 1], dh
    pop bx
    pop ds

    call VideoPlaceCursor
    iret

Int10GetCursor:
    push ds
    push bx
    mov bx, BDA_SEGMENT
    mov ds, bx
    mov dl, byte ptr ds:[BDA_CURSOR]
    mov dh, byte ptr ds:[BDA_CURSOR + 1]
    pop bx
    pop ds

    /* The shape it is drawn with, which nothing here varies */
    mov ch, 6
    mov cl, 7
    iret

Int10QueryMode:
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

/* What is under the cursor, which something drawing a box asks before it draws */
Int10ReadCell:
    push ds
    push si
    call VideoCursorOffset
    mov si, ax
    mov ax, VIDEO_SEGMENT
    mov ds, ax
    mov ax, word ptr ds:[si]
    pop si
    pop ds
    iret

/*
 * A character put where the cursor is, without moving it. The count in CX is
 * how many times, which is how a line of one character is drawn.
 */
Int10WriteCell:
    push ax
    push bx
    push cx
    push di
    push es

    push ax
    call VideoCursorOffset
    mov di, ax
    pop ax

    /*
     * The segment goes in through AX, not BX. BL is the colour the caller asked
     * for and there is nowhere else it is kept.
     */
    push ax
    mov ax, VIDEO_SEGMENT
    mov es, ax
    pop ax

    /* At least one, however little CX says */
    test cx, cx
    jnz Int10WriteCellLoop
    mov cx, 1

Int10WriteCellLoop:
    mov byte ptr es:[di], al

    /*
     * The colour is only taken from BL by the form that carries one. The other
     * leaves whatever the cell already had, which is what it is for.
     */
    cmp ah, HEX(09)
    jne Int10WriteCellNext
    mov byte ptr es:[di + 1], bl

Int10WriteCellNext:
    add di, 2
    loop Int10WriteCellLoop

    pop es
    pop di
    pop cx
    pop bx
    pop ax
    iret

/*
 * A whole line at once, put where it is told rather than where the cursor is.
 *
 * What draws a menu uses this and almost nothing else: it clears the screen,
 * paints the bar over the choice that is picked, and then puts every line of
 * words down with one of these. A firmware without it shows the clearing and
 * the bar and none of the words, which looks like a menu that went wrong
 * rather than like a call that is missing.
 *
 * AL says how it is being asked: the low bit whether the cursor is left where
 * the line ended, and the one above it whether the caller's string carries a
 * colour with every character or one colour in BL for the whole of it.
 */
Int10WriteString:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push ds
    push es

    /* Where it goes, which is a row and a column rather than an offset */
    push ax
    mov ax, dx
    call VideoPlaceOffset
    mov di, ax
    pop ax

    /* The caller's string, and the page this firmware draws */
    mov si, bp
    push es
    pop ds

    push ax
    mov ax, VIDEO_SEGMENT
    mov es, ax
    pop ax

    /* Kept, because the loop below needs both halves of it and AX is in use */
    mov byte ptr cs:[StringManner], al
    mov byte ptr cs:[StringColour], bl

    test cx, cx
    jz Int10WriteStringDone

Int10WriteStringNext:
    lodsb

    /*
     * Said out of the serial port as well, the way every other character this
     * firmware draws is. A machine with nothing looking at its screen is
     * watched through that port, and a menu put down with this call rather
     * than one character at a time would otherwise say nothing there at all.
     */
    call SerialWrite

    /* The few that mean something rather than standing for themselves */
    cmp al, 13
    je Int10WriteStringReturn
    cmp al, 10
    je Int10WriteStringFeed
    cmp al, 8
    je Int10WriteStringBack
    cmp al, 7
    je Int10WriteStringSkip

    mov byte ptr es:[di], al

    /* A colour of its own follows each character, or one covers them all */
    test byte ptr cs:[StringManner], 2
    jz Int10WriteStringOneColour

    lodsb
    mov byte ptr es:[di + 1], al
    jmp Int10WriteStringOn

Int10WriteStringOneColour:
    mov al, byte ptr cs:[StringColour]
    mov byte ptr es:[di + 1], al

Int10WriteStringOn:
    add di, 2

Int10WriteStringSkip:
    dec cx
    jnz Int10WriteStringNext
    jmp Int10WriteStringDone

/* Back to the start of the row this one is on */
Int10WriteStringReturn:
    push ax
    push dx
    mov ax, di
    xor dx, dx
    div word ptr cs:[StringRowBytes]
    mul word ptr cs:[StringRowBytes]
    mov di, ax
    pop dx
    pop ax
    jmp Int10WriteStringSkip

/* On by one whole row, leaving the column where it is */
Int10WriteStringFeed:
    add di, VIDEO_COLUMNS * 2
    jmp Int10WriteStringSkip

/* Back by one cell, but never off the front of the page */
Int10WriteStringBack:
    cmp di, 2
    jb Int10WriteStringSkip
    sub di, 2
    jmp Int10WriteStringSkip

Int10WriteStringDone:
    /*
     * Left where the line ended, but only if the caller asked for that. The
     * low bit of how it asked is the whole of the difference: a menu draws
     * every line where it says and never looks at the cursor, and something
     * printing a run of text carries on from where the last one stopped.
     */
    test byte ptr cs:[StringManner], 1
    jz Int10WriteStringLeave

    push ax
    push bx
    push cx
    push dx
    push ds

    /* Which row and column that offset is, which is how a cursor is kept */
    mov ax, di
    shr ax, 1
    xor dx, dx
    mov cx, VIDEO_COLUMNS
    div cx

    mov bx, BDA_SEGMENT
    mov ds, bx
    mov byte ptr ds:[BDA_CURSOR], dl
    mov byte ptr ds:[BDA_CURSOR + 1], al

    pop ds
    call VideoPlaceCursor

    pop dx
    pop cx
    pop bx
    pop ax

Int10WriteStringLeave:
    pop es
    pop ds
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    iret

StringManner:
    .byte HEX(00)

StringColour:
    .byte HEX(07)

StringSpare:
    .byte HEX(00)

StringRowBytes:
    .word VIDEO_COLUMNS * 2

/*
 * Where the row in AH and the column in AL land in the page, in bytes.
 */
VideoPlaceOffset:
    push bx
    push dx

    movzx bx, al
    movzx ax, ah
    mov dx, VIDEO_COLUMNS
    mul dx
    add ax, bx
    shl ax, 1

    pop dx
    pop bx
    ret

/*
 * One character, at the cursor, and the cursor moves. This is the call almost
 * everything uses, and the only one that has to understand what a newline is.
 */
Int10Teletype:
    push ax
    push bx
    push cx
    push dx
    push di
    push ds
    push es

    /* Out of the port as well, so the machine can be watched */
    push ax
    call SerialWrite
    pop ax

    mov bx, BDA_SEGMENT
    mov ds, bx

    cmp al, 13
    je Int10Return
    cmp al, 10
    je Int10LineFeed
    cmp al, 8
    je Int10Backspace
    cmp al, 7
    je Int10Bell

    /* An ordinary character goes in the cell the cursor is on */
    push ax
    call VideoCursorOffset
    mov di, ax
    pop ax

    mov bx, VIDEO_SEGMENT
    mov es, bx
    mov byte ptr es:[di], al
    mov byte ptr es:[di + 1], HEX(07)

    mov bx, BDA_SEGMENT
    mov ds, bx

    /* On one, and round onto the next line at the edge */
    inc byte ptr ds:[BDA_CURSOR]
    cmp byte ptr ds:[BDA_CURSOR], VIDEO_COLUMNS
    jb Int10TeletypeDone

Int10LineFeed:
    mov byte ptr ds:[BDA_CURSOR], 0
    inc byte ptr ds:[BDA_CURSOR + 1]
    cmp byte ptr ds:[BDA_CURSOR + 1], VIDEO_ROWS
    jb Int10TeletypeDone

    /* Off the bottom, so everything moves up and the last line is the new one */
    mov byte ptr ds:[BDA_CURSOR + 1], VIDEO_ROWS - 1
    mov bh, HEX(07)
    call VideoScroll
    jmp Int10TeletypeDone

Int10Return:
    mov byte ptr ds:[BDA_CURSOR], 0
    jmp Int10TeletypeDone

Int10Backspace:
    cmp byte ptr ds:[BDA_CURSOR], 0
    je Int10TeletypeDone
    dec byte ptr ds:[BDA_CURSOR]
    jmp Int10TeletypeDone

Int10Bell:
    /* Nothing to ring */

Int10TeletypeDone:
    call VideoPlaceCursor

    pop es
    pop ds
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    iret

#include "scroll.S"

/* WHERE THE CURSOR IS ********************************************************/

/* Answers in AX with the offset into the page of the cell the cursor is on */
VideoCursorOffset:
    push bx
    push cx
    push dx
    push ds

    mov bx, BDA_SEGMENT
    mov ds, bx

    movzx ax, byte ptr ds:[BDA_CURSOR + 1]
    mov cx, VIDEO_COLUMNS
    mul cx
    movzx bx, byte ptr ds:[BDA_CURSOR]
    add ax, bx
    shl ax, 1

    pop ds
    pop dx
    pop cx
    pop bx
    ret

/* Tells the display where to draw the cursor, which it keeps in two registers */
VideoPlaceCursor:
    push ax
    push bx
    push cx
    push dx
    push ds

    mov bx, BDA_SEGMENT
    mov ds, bx

    movzx ax, byte ptr ds:[BDA_CURSOR + 1]
    mov cx, VIDEO_COLUMNS
    mul cx
    movzx bx, byte ptr ds:[BDA_CURSOR]
    add ax, bx

    mov dx, VIDEO_CRTC_ADDRESS
    mov al, HEX(0E)
    out dx, al
    inc dx
    mov al, ah
    out dx, al

    dec dx
    mov al, HEX(0F)
    out dx, al
    inc dx
    /* The low half, which the multiply left in AL before AH was taken */
    movzx ax, byte ptr ds:[BDA_CURSOR + 1]
    mov cx, VIDEO_COLUMNS
    mul cx
    movzx bx, byte ptr ds:[BDA_CURSOR]
    add ax, bx
    out dx, al

    pop ds
    pop dx
    pop cx
    pop bx
    pop ax
    ret

/* Empties the page, in the colour a screen comes up in */
VideoClear:
    push ax
    push cx
    push di
    push es

    mov ax, VIDEO_SEGMENT
    mov es, ax
    xor di, di
    mov cx, VIDEO_COLUMNS * VIDEO_ROWS
    mov ax, HEX(0720)
    cld
    rep stosw

    pop es
    pop di
    pop cx
    pop ax
    ret

/* Everything up one line, the last one emptied in the colour BH names */
VideoScroll:
    push ax
    push cx
    push si
    push di
    push ds
    push es

    mov ax, VIDEO_SEGMENT
    mov ds, ax
    mov es, ax

    mov si, VIDEO_COLUMNS * 2
    xor di, di
    mov cx, VIDEO_COLUMNS * (VIDEO_ROWS - 1)
    cld
    rep movsw

    /* The line that has just been made room for */
    mov cx, VIDEO_COLUMNS
    mov ah, bh
    mov al, HEX(20)
    rep stosw

    pop es
    pop ds
    pop di
    pop si
    pop cx
    pop ax
    ret

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
     * Which drive is being asked for. Anything with the top bit down is a
     * removable one and goes to the controller that has those; the fixed disk
     * is the only other thing here.
     */
    cmp dl, HEX(E0)
    je Int13Disc
    cmp dl, HEX(80)
    je Int13OurDrive
    test dl, HEX(80)
    jz Int13Removable
    jmp Int13NotOurDrive

/*
 * The drive a disc is in, which is asked for by number rather than by shape
 * and answers a different handful of calls from everything else.
 */
Int13Disc:
    cmp ah, HEX(00)
    je Int13DiscReset
    cmp ah, HEX(41)
    je Int13DiscExtensions
    cmp ah, HEX(42)
    je Int13CdExtendedRead
    cmp ah, HEX(48)
    je Int13CdParameters
    cmp ah, HEX(4B)
    je Int13DiscBootSpec
    cmp ah, HEX(15)
    je Int13DiscType
    jmp Int13NotSupported

Int13DiscReset:
    xor ah, ah
    jmp Int13Succeeded

Int13DiscExtensions:
    /* The word anything asks for before believing the answer */
    mov bx, HEX(AA55)
    mov ah, HEX(21)
    mov cx, HEX(0005)
    jmp Int13Succeeded

Int13DiscBootSpec:
    cmp al, HEX(01)
    jne Int13NotSupported
    jmp Int13CdSpec

Int13DiscType:
    /* One that can tell it has been changed, which is what the two means */
    mov ah, HEX(02)
    jmp Int13Succeeded

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

/* THE REMOVABLE DRIVE ********************************************************/

Int13Removable:
    cmp dl, 0
    jne Int13NotOurDrive

    /*
     * A disc that is pretending to be this drive answers for it instead. What
     * asks is entitled to find a drive of this number there, and whether the
     * bytes come off a removable disk or off a disc is not its business.
     */
    call CdIsPretending
    jc Int13FloppyItself

    cmp ah, HEX(00)
    je Int13PretendReset
    cmp ah, HEX(02)
    je Int13PretendRead
    cmp ah, HEX(08)
    je Int13PretendParameters
    cmp ah, HEX(15)
    je Int13PretendType
    jmp Int13NotSupported

Int13FloppyItself:
    cmp ah, HEX(00)
    je Int13FloppyReset
    cmp ah, HEX(02)
    je Int13FloppyRead
    cmp ah, HEX(08)
    je Int13FloppyParameters
    cmp ah, HEX(15)
    je Int13FloppyType
    jmp Int13NotSupported

Int13PretendReset:
    xor ah, ah
    jmp Int13Succeeded

/* One that can tell it has been changed, which is what the two means */
Int13PretendType:
    mov ah, HEX(02)
    jmp Int13Succeeded

/*
 * The shape of the drive being pretended. What asks for this believes every
 * number in it and then asks for sectors that far round, so the shape said
 * here has to be the one the sums in the reader are done with.
 */
Int13PretendParameters:
    push cx
    push dx

    call CdPretendRound
    mov cl, al
    and cl, HEX(3F)
    mov ch, CD_PRETEND_TRACKS - 1
    mov dh, CD_PRETEND_SIDES - 1
    mov dl, 1

    mov word ptr cs:[PretendShape], cx
    mov word ptr cs:[PretendSides], dx

    pop dx
    pop cx

    mov cx, word ptr cs:[PretendShape]
    mov dx, word ptr cs:[PretendSides]

    mov bl, HEX(04)
    xor ah, ah
    jmp Int13Succeeded

/*
 * A run of its sectors into ES:BX. Every one of them is a quarter of one of
 * the disc's and is fetched on its own, because a run that was asked for in
 * one go would still be four separate places on the disc.
 */
Int13PretendRead:
    push ax
    push bx
    push cx
    push dx
    push si

    movzx si, al

Int13PretendNext:
    test si, si
    jz Int13PretendDone

    /*
     * Which sector it is comes back in the pair of registers that also hold
     * which side of the drive is being read, so where it was has to be put
     * away first or the sector after this one is looked for on a side the
     * drive does not have.
     */
    push cx
    push dx
    call CdPretendPlace

    call CdPretendRead
    pop dx
    pop cx
    jc Int13PretendFailed

    /* On by one of its sectors, and round to the next place on the drive */
    add bx, CD_PRETEND_SECTOR
    call CdPretendStep

    dec si
    jmp Int13PretendNext

Int13PretendDone:
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    xor ah, ah
    jmp Int13Succeeded

Int13PretendFailed:
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    mov ah, HEX(04)
    jmp Int13Failed

Int13FloppyReset:
    call FloppyStart
    jc Int13Failed
    xor ah, ah
    jmp Int13Succeeded

Int13FloppyRead:
    push bx
    push cx
    push dx

    call FloppyStart
    jc Int13FloppyGone

    call FloppyReadSectors
    jc Int13FloppyGone

    call FloppyStop

    pop dx
    pop cx
    pop bx

    /* AL comes back saying how many were moved, which is all of them or none */
    xor ah, ah
    jmp Int13Succeeded

Int13FloppyGone:
    call FloppyStop

    pop dx
    pop cx
    pop bx

    mov ah, HEX(04)
    jmp Int13Failed

/*
 * The shape of the medium, which is the one shape anything still makes. The
 * numbers are one less than the count, the way this call has always given them.
 */
Int13FloppyParameters:
    mov ah, HEX(00)
    mov bl, HEX(04)
    mov ch, 79
    mov cl, 18
    mov dh, 1
    mov dl, 1
    push cs
    pop es
    mov di, OFFSET FloppyParameterTable
    jmp Int13Succeeded

Int13FloppyType:
    /* One that can tell it has been changed, which is what the two means */
    mov ah, HEX(02)
    jmp Int13Succeeded

/*
 * What a driver reads instead of asking. Everything in it is what the medium
 * above is written with.
 */
FloppyParameterTable:
    .byte HEX(AF)
    .byte HEX(02)
    .byte HEX(25)
    .byte HEX(02)
    .byte 18
    .byte HEX(1B)
    .byte HEX(FF)
    .byte HEX(6C)
    .byte HEX(F6)
    .byte HEX(0F)
    .byte HEX(08)

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
    cmp ax, HEX(E801)
    je Int15SizeInParts
    cmp ah, HEX(88)
    je Int15ExtendedSize

    /* Anything else is refused, and the caller decides what that means */
    stc
    mov ah, HEX(86)
    retf 2

/*
 * Kilobytes above the first megabyte as one number, which stops at the largest
 * a word holds. A machine with more than that is why the other two calls are
 * here at all, so clamping rather than wrapping is the only honest answer.
 */
Int15ExtendedSize:
    push edx

    mov eax, dword ptr cs:[MemorySizeLow]
    mov edx, dword ptr cs:[MemorySizeHigh]
    sub eax, HEX(00100000)
    sbb edx, 0
    jc Int15ExtendedNone

    shr eax, 10
    test edx, edx
    jnz Int15ExtendedFull
    cmp eax, HEX(0000FFFF)
    jbe Int15ExtendedReady

Int15ExtendedFull:
    mov eax, HEX(0000FFFF)

Int15ExtendedReady:
    pop edx
    clc
    retf 2

Int15ExtendedNone:
    pop edx
    xor ax, ax
    stc
    mov ah, HEX(86)
    retf 2

/*
 * The same size told in two parts: kilobytes up to sixteen megabytes, then
 * blocks of sixty four above it. Both pairs of registers get the same pair of
 * numbers, which is what a caller that prefers either one of them expects.
 */
Int15SizeInParts:
    push esi
    push edi

    mov eax, dword ptr cs:[MemorySizeLow]
    mov edx, dword ptr cs:[MemorySizeHigh]

    /* Anything past four gigabytes is told as being just short of it */
    test edx, edx
    jz Int15PartsHave
    mov eax, HEX(FFFF0000)

Int15PartsHave:
    cmp eax, HEX(00100000)
    jbe Int15PartsNone

    /* What of it falls below sixteen megabytes, counted in kilobytes */
    mov esi, eax
    cmp esi, HEX(01000000)
    jbe Int15PartsLowReady
    mov esi, HEX(01000000)

Int15PartsLowReady:
    sub esi, HEX(00100000)
    shr esi, 10

    /* And what of it lies above, counted in the larger unit */
    xor edi, edi
    cmp eax, HEX(01000000)
    jbe Int15PartsHighReady
    mov edi, eax
    sub edi, HEX(01000000)
    shr edi, 16

Int15PartsHighReady:
    mov ax, si
    mov cx, si
    mov bx, di
    mov dx, di
    pop edi
    pop esi
    clc
    retf 2

Int15PartsNone:
    pop edi
    pop esi
    stc
    mov ah, HEX(86)
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
    push di

    push cs
    pop ds

    mov si, OFFSET MemoryMap
    mov ax, bx
    mov cx, 20
    mul cx
    add si, ax

    /* Forwards, whichever way round the caller happened to leave it */
    cld
    mov cx, 20
Int15MapCopy:
    lodsb
    stosb
    loop Int15MapCopy

    pop di
    pop cx
    pop si
    pop ds

    /* Which one to ask for next, counted before anything is returned in it */
    inc bx
    movzx ax, byte ptr cs:[MemoryMapCount]
    cmp bx, ax
    jb Int15MapMore

    /* The last one is answered with zero, which says not to ask again */
    xor bx, bx

/*
 * The name goes back last because the count was worked out in the same
 * register, and a caller that does not see it there stops reading the map.
 */
Int15MapMore:
    mov eax, HEX(534D4150)
    mov ecx, 20
    clc
    retf 2

/* Asked for one past the end, which is refused rather than answered */
Int15MapDone:
    mov eax, HEX(534D4150)
    xor bx, bx
    stc
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

    /*
     * The wider forms of the same three calls. Anything written since keys had
     * more of them on the keyboard asks for these, and a handler that simply
     * returns from one leaves the flags holding whatever they held: the caller
     * reads that as a key being ready and then asks for a key that is not
     * there, which is a wait that never ends.
     */
    cmp ah, HEX(10)
    je Int16Read
    cmp ah, HEX(11)
    je Int16Peek
    cmp ah, HEX(12)
    je Int16Flags

    /* Setting the rate keys repeat at, which there is nothing to set */
    cmp ah, HEX(03)
    je Int16Accepted

    /* Anything else is refused plainly rather than by saying nothing */
    xor ax, ax
    jmp Int16Empty

Int16Accepted:
    iret

/*
 * Taking a character, waiting for one if there is none.
 *
 * A character that was already looked at is handed over from where the look
 * left it, because a port read takes the byte off the wire and there is
 * nowhere else for it to go.
 */
Int16Read:
    push dx

Int16Wait:
    cmp byte ptr cs:[KeyWaiting], 0
    jne Int16FromHeld

    /* Nothing arrives that way on a machine with no port there */
    cmp byte ptr cs:[Com1There], 0
    je Int16Rest

    mov dx, COM1_BASE + UART_LINE_STATUS
    in al, dx
    test al, HEX(01)
    jnz Int16Take

Int16Rest:
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

Int16FromHeld:
    mov al, byte ptr cs:[KeyHeld]
    mov ah, byte ptr cs:[KeyCode]
    mov byte ptr cs:[KeyWaiting], 0
    pop dx
    iret

/*
 * Looking without taking. The port has no way to be read without taking, so
 * the byte is taken and held here until the read that follows asks for it.
 * Answering this by reading and discarding is how a menu that polls before
 * reading loses every key that is pressed at it.
 */
Int16Peek:
    /*
     * Interrupts back on first. The gate this arrived through turned them off,
     * and something polling this in a loop would then spend nearly all its time
     * unable to take a clock tick, which is a machine whose time stands still
     * for as long as it is waiting for a key.
     */
    sti

    cmp byte ptr cs:[KeyWaiting], 0
    jne Int16HaveOne

    push dx

    /* The same, because a port that is not there always says it has one */
    cmp byte ptr cs:[Com1There], 0
    je Int16PeekEmpty

    mov dx, COM1_BASE + UART_LINE_STATUS
    in al, dx
    test al, HEX(01)
    jz Int16PeekEmpty

    mov dx, COM1_BASE + UART_DATA
    in al, dx
    mov byte ptr cs:[KeyHeld], al

    /* No scan code to give, and a stale one reads as a key of its own */
    mov byte ptr cs:[KeyCode], 0
    mov byte ptr cs:[KeyWaiting], 1
    pop dx

Int16HaveOne:
    mov al, byte ptr cs:[KeyHeld]
    mov ah, byte ptr cs:[KeyCode]

    /* Clear, meaning there is one */
    push bp
    mov bp, sp
    and word ptr [bp + 6], NOT HEX(40)
    pop bp
    iret

Int16PeekEmpty:
    pop dx

Int16Empty:
    push bp
    mov bp, sp
    or word ptr [bp + 6], HEX(40)
    pop bp
    iret

Int16Flags:
    sti
    xor al, al
    iret

/* The clock, kept by nothing yet, so the count simply stands still */
Int1AHandler:
    cmp ah, HEX(00)
    je Int1AReadTicks
    cmp ah, HEX(02)
    je Int1AReadTime
    cmp ah, HEX(04)
    je Int1AReadDate
    cmp ah, HEX(01)
    je Int1AWriteTicks

    /* Setting either half of the clock, which nothing here lets a guest do */
    cmp ah, HEX(03)
    je Int1ASucceeded
    cmp ah, HEX(05)
    je Int1ASucceeded

    jmp Int1AFailed

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

Int1AWriteTicks:
    push ds
    push bx
    mov bx, BDA_SEGMENT
    mov ds, bx
    mov word ptr ds:[BDA_TICKS], dx
    mov word ptr ds:[BDA_TICKS + 2], cx
    pop bx
    pop ds
    jmp Int1ASucceeded

/*
 * What the clock says, which is asked of the clock rather than remembered.
 *
 * Anything counting down a menu watches the seconds here, so a handler that
 * answered with the second the machine started in would have it count nothing
 * down at all.
 */
Int1AReadTime:
    push ax

    mov al, CMOS_HOURS
    call CmosTake
    mov ch, al

    mov al, CMOS_MINUTES
    call CmosTake
    mov cl, al

    mov al, CMOS_SECONDS
    call CmosTake
    mov dh, al

    /* Nothing here moves the clock for the summer */
    xor dl, dl

    pop ax
    jmp Int1ASucceeded

Int1AReadDate:
    push ax

    mov al, CMOS_CENTURY
    call CmosTake
    mov ch, al

    mov al, CMOS_YEAR
    call CmosTake
    mov cl, al

    mov al, CMOS_MONTH
    call CmosTake
    mov dh, al

    mov al, CMOS_DAY
    call CmosTake
    mov dl, al

    pop ax
    jmp Int1ASucceeded

/*
 * One of the clock's own registers, named first and then read.
 *
 * The saved flags are what the caller ends up with, so clearing the carry in
 * the register itself would be lost the moment this returns through the gate.
 */
Int1ASucceeded:
    push bp
    mov bp, sp
    and word ptr [bp + 6], NOT 1
    pop bp
    iret

Int1AFailed:
    push bp
    mov bp, sp
    or word ptr [bp + 6], 1
    pop bp
    iret

CmosTake:
    out CMOS_ADDRESS, al

    /* A moment between naming one and reading it, which the part wants */
    jmp CmosSettled
CmosSettled:
    in al, CMOS_DATA
    ret

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

    /*
     * Which drive, first of all. Waiting before selecting asks whichever drive
     * was last spoken to whether it is ready, and the registers of a drive
     * that is not there read as nothing: the ready bit never appears and the
     * wait times out forever. Something else probing the other drive on the
     * cable is enough to leave it selected, so this order is not optional.
     */
    mov dx, ATA_PRIMARY_BASE + ATA_DRIVE
    mov al, byte ptr cs:[AtaHead]
    and al, HEX(0F)
    or al, HEX(A0)
    cmp byte ptr cs:[AtaUseLba], 0
    je AtaDriveReady
    or al, HEX(40)
AtaDriveReady:
    out dx, al

    call AtaWaitReady
    jc AtaReadFailed

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

    mov dx, ATA_PRIMARY_BASE + ATA_DRIVE
    mov al, HEX(A0)
    out dx, al

    call AtaWaitReady
    jc AtaIdentifyFailed

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
/*
 * The removable drive saying it has finished. Nothing is read here: what the
 * command left behind is taken by whoever asked for it, and all this does is
 * let them stop waiting.
 */
Int0EHandler:
    push ax
    push ds

    mov ax, BDA_SEGMENT
    mov ds, ax
    mov byte ptr ds:[BDA_FLOPPY_DONE], 1

    mov al, HEX(20)
    out HEX(20), al

    pop ds
    pop ax
    iret

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

/* BOOTING FROM A DISC ********************************************************/

#include "cdrom.S"

/* TAKING KEYS ****************************************************************/

#include "vga.S"
#include "vesa.S"

#include "keyboard.S"

/* DRIVING THE REMOVABLE DRIVE ************************************************/

#include "floppy.S"

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
    call VideoClear
    call ControllerSetup
    call SerialProbe

    /* Before anything is driven, because nothing on the bus answers until it */
    call BusSetup

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

    mov bx, PIC_MASTER_VECTOR + 6
    mov ax, OFFSET Int0EHandler
    call VectorSet

    mov bx, PIC_MASTER_VECTOR + 1
    mov ax, OFFSET Int09Handler
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
    /* One short of the round number, the last kilobyte being spoken for */
    mov word ptr ds:[BDA_MEMORY_SIZE], 639

    mov byte ptr ds:[BDA_KEYBOARD_FLAGS], 0
    mov word ptr ds:[BDA_KEYBOARD_HEAD], HEX(001E)
    mov word ptr ds:[BDA_KEYBOARD_TAIL], HEX(001E)

    mov byte ptr ds:[BDA_DISPLAY_MODE], 3
    mov word ptr ds:[BDA_DISPLAY_COLUMNS], 80
    mov byte ptr ds:[BDA_DISPLAY_PAGE], 0

    /* Held one short, which is how everything that reads it expects it */
    mov byte ptr ds:[BDA_DISPLAY_ROWS], VIDEO_ROWS - 1

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
     * Open the lines something is actually listening for: the timer, the
     * keys, the way the second controller arrives, the serial port and the
     * removable drive. A line nothing answers is worse than no line at all,
     * because the controller is left holding it and never delivers another.
     */
    mov al, HEX(A8)
    out PIC_MASTER_DATA, al

    /* On the second one, the disk */
    mov al, HEX(BF)
    out PIC_SLAVE_DATA, al

    pop ax
    ret

/* THE BUS *******************************************************************/

/*
 * Everything on the bus comes up answering nowhere. A device says how much of
 * each kind of address it wants, and until something has read that and written
 * back where it may answer, it is on the bus and unreachable.
 *
 * That is this firmware's to do. What runs later finds the same device at the
 * same place and keeps what was handed out, because a device that has already
 * been given somewhere to answer is one less thing for it to decide.
 */

/*
 * The four bytes at the register in AL of whatever ESI is pointing at, into
 * EAX. ESI carries which place on the bus is meant and is left alone.
 */
PciRead:
    push dx
    push ebx

    movzx ebx, al
    and bl, HEX(FC)
    or ebx, esi

    mov eax, ebx
    mov dx, PCI_ADDRESS_PORT
    out dx, eax

    mov dx, PCI_DATA_PORT
    in eax, dx

    pop ebx
    pop dx
    ret

/* And ECX into that same register */
PciWrite:
    push eax
    push dx
    push ebx

    movzx ebx, al
    and bl, HEX(FC)
    or ebx, esi

    mov eax, ebx
    mov dx, PCI_ADDRESS_PORT
    out dx, eax

    mov dx, PCI_DATA_PORT
    mov eax, ecx
    out dx, eax

    pop ebx
    pop dx
    pop eax
    ret

/* Where the device in CX and the part of it in DX are, packed into ESI */
PciAddress:
    push eax

    movzx esi, cx
    shl esi, 11
    movzx eax, dx
    shl eax, 8
    or esi, eax
    or esi, HEX(80000000)

    pop eax
    ret

/*
 * Gives one device somewhere to answer and turns it on.
 *
 * How much of each kind it wants is not written down: a device is written all
 * ones and read back, and what comes back has zeroes wherever the address is
 * fixed. The lowest bit still standing is both how much there is of it and how
 * far apart two of them have to be, which is why the same number does for
 * rounding up and for moving on.
 *
 * DI is where the next run of ports goes and EBP the next run of memory. Both
 * are carried through, so the whole bus is handed out from one pair of numbers.
 */
PciPlace:
    push eax
    push ebx
    push ecx
    push dx

    mov bl, PCI_FIRST_RANGE

PciRange:
    mov al, bl
    mov ecx, HEX(FFFFFFFF)
    call PciWrite

    mov al, bl
    call PciRead
    mov ecx, eax

    /* Nothing asked for at all */
    test ecx, ecx
    jz PciNextRange

    test cl, 1
    jz PciRangeMemory

    /* A run of ports, whose lowest two bits are not part of the answer */
    and ecx, HEX(0000FFFC)
    jz PciNextRange

    mov ax, cx
    neg ax
    and ax, cx
    mov dx, ax

    dec ax
    add di, ax
    not ax
    and di, ax

    movzx ecx, di
    or ecx, 1
    mov al, bl
    call PciWrite

    add di, dx
    jmp PciNextRange

PciRangeMemory:
    and ecx, HEX(FFFFFFF0)
    jz PciNextRange

    mov eax, ecx
    neg eax
    and eax, ecx
    push eax

    dec eax
    add ebp, eax
    not eax
    and ebp, eax

    mov ecx, ebp
    mov al, bl
    call PciWrite

    pop eax
    add ebp, eax

PciNextRange:
    add bl, 4
    cmp bl, PCI_PAST_RANGES
    jb PciRange

    /* Now that it has somewhere to answer, it is told it may */
    mov al, PCI_DOING
    call PciRead
    mov ecx, eax
    or cl, PCI_DECODE
    mov al, PCI_DOING
    call PciWrite

    /* And the one thing this board knows that the bus does not say */
    mov al, PCI_WHAT
    call PciRead
    shr eax, 16
    cmp ax, PCI_IS_DISK
    jne PciPlaced

    mov ecx, PCI_DISK_DECODES
    mov al, PCI_DISK_TIMING
    call PciWrite

PciPlaced:
    pop dx
    pop ecx
    pop ebx
    pop eax
    ret

BusSetup:
    push eax
    push ebx
    push ecx
    push edx
    push esi
    push edi
    push ebp

    mov di, PCI_FIRST_PORT
    mov ebp, PCI_FIRST_MEMORY

    xor cx, cx

BusDevice:
    xor dx, dx
    call PciAddress

    mov al, PCI_WHO
    call PciRead
    cmp ax, PCI_NOBODY
    je BusNextDevice

    call PciPlace

    /* Whether there is more to this place than the part just dealt with */
    mov al, PCI_LAYOUT
    call PciRead
    shr eax, 16
    test al, PCI_HAS_MORE
    jz BusNextDevice

    mov dx, 1

BusPart:
    call PciAddress

    mov al, PCI_WHO
    call PciRead
    cmp ax, PCI_NOBODY
    je BusNextPart

    call PciPlace

BusNextPart:
    inc dx
    cmp dx, 8
    jb BusPart

BusNextDevice:
    inc cx
    cmp cx, 16
    jb BusDevice

    pop ebp
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop eax
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

    /*
     * A disc first, then the removable drive, then the fixed disk. Something
     * an operator put in the machine is what they wanted booted, and the fixed
     * disk is what is there the rest of the time.
     */
    call CdBoot
    jnc Int19FromDisc

    mov byte ptr cs:[BootDrive], HEX(00)
    call Int19TryDrive
    jnc Int19Bootable

    cmp byte ptr cs:[DiskPresent], 0
    je Int19NoDisk

    mov byte ptr cs:[BootDrive], HEX(80)
    call Int19TryDrive
    jc Int19NotBootable

Int19FromDisc:
    /*
     * What came off a disc is entered with the number a drive of that kind is
     * given, because that is the one thing it is entitled to assume and the
     * only way it can ask for the rest of itself.
     *
     * Unless the disc was pretending to be a removable drive, in which case
     * what was loaded was written long before discs and knows only that
     * number. Handing it the disc's would have it ask a drive it has never
     * heard of for tracks and sides, and be told there are none.
     */
    mov byte ptr cs:[BootDrive], HEX(E0)

    call CdIsPretending
    jc Int19Bootable

    mov byte ptr cs:[BootDrive], HEX(00)

Int19Bootable:
    push cs
    pop ds
    mov si, OFFSET HandOverMessage
    call SerialWriteString
    jmp Int19HandOver

/*
 * Reads the first sector off whichever drive BootDrive names, and says whether
 * what came back is something worth jumping into.
 */
Int19TryDrive:
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
    jc Int19TryFailed

    /* Is it one */
    mov ax, BOOT_SEGMENT
    mov es, ax
    cmp word ptr es:[BOOT_OFFSET + 510], HEX(AA55)
    jne Int19TryFailed

    clc
    ret

Int19TryFailed:
    stc
    ret

Int19HandOver:

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

CrLfMessage:
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
PretendShape:
    .word HEX(0000)

PretendSides:
    .word HEX(0000)

BootDrive:
    .byte HEX(80)
KeyWaiting:
    .byte 0
KeyHeld:
    .byte 0
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
