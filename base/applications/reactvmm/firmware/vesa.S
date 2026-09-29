/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The calls a caller makes when a screen of characters is not enough
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * A display of this kind has more modes than there were numbers left for, so
 * they are asked for through one call that takes a whole word rather than
 * through the byte the old one had. A caller asks what the display can do, asks
 * about a mode it likes the look of, and then asks for it.
 *
 * What comes back about a mode is mostly where the pixels are. Everything a
 * caller does afterwards it does by writing there directly, which is the whole
 * reason these calls exist: a screen this size cannot be drawn a byte at a time
 * through a window sixty four thousand bytes wide.
 *
 * The display itself is told what to be by writing its registers, the same ones
 * that have always said how wide and how tall a picture is, plus the handful it
 * has of its own for saying how much of a pixel there is and where the whole of
 * the memory was put. This firmware does not decide where that is: it reads it
 * back out of the display, because where a card answers is the board's to say.
 */

/* The two ports every one of the display's registers is reached through */
#define VESA_CRTC_ADDRESS   HEX(03D4)
#define VESA_SEQ_ADDRESS    HEX(03C4)

/* The ones every display has had since before any of this */
#define VESA_CRTC_WIDTH_END     HEX(01)
#define VESA_CRTC_OVERFLOW      HEX(07)
#define VESA_CRTC_MAX_ROW       HEX(09)
#define VESA_CRTC_START_HIGH    HEX(0C)
#define VESA_CRTC_START_LOW     HEX(0D)
#define VESA_CRTC_HEIGHT_END    HEX(12)
#define VESA_CRTC_ROW_LENGTH    HEX(13)
#define VESA_CRTC_BLANK_START   HEX(15)

/* And the ones this kind of display has of its own */
#define VESA_CRTC_MEMORY_CONFIG     HEX(31)
#define VESA_CRTC_UNLOCK_ONE        HEX(38)
#define VESA_CRTC_UNLOCK_TWO        HEX(39)
#define VESA_CRTC_MODE_CONTROL      HEX(42)
#define VESA_CRTC_EXTENDED_MODE     HEX(43)
#define VESA_CRTC_EXTENDED_SYSTEM   HEX(51)
#define VESA_CRTC_LINEAR_CONTROL    HEX(58)
#define CRTC_LINEAR_HIGH            HEX(59)
#define CRTC_LINEAR_LOW             HEX(5A)
#define VESA_CRTC_EXTENDED_WIDTH    HEX(5D)
#define VESA_CRTC_EXTENDED_HEIGHT   HEX(5E)
#define VESA_CRTC_DAC_MODE          HEX(67)
#define VESA_CRTC_EXTENDED_START    HEX(69)

/* Where in one row of the table each thing about a mode is */
#define VBE_TABLE_NUMBER    0
#define VBE_TABLE_WIDTH     2
#define VBE_TABLE_HEIGHT    4
#define VBE_TABLE_DEPTH     6
#define VBE_TABLE_LENGTH    8

/* What a caller checks for before believing any of this */
#define VBE_SUPPORTED       HEX(004F)
#define VBE_SIGNATURE       HEX(41534556)    /* VESA */
#define VBE_VERSION         HEX(0200)

/* How much of a screen this display has to draw out of, in lots of 64K */
#define VBE_MEMORY_BLOCKS   64

/* Where in the block a caller is handed each thing about the display */
#define VBE_INFO_SIGNATURE  0
#define VBE_INFO_VERSION    4
#define VBE_INFO_MAKER      6
#define VBE_INFO_CAN_DO     10
#define VBE_INFO_MODES      14
#define VBE_INFO_MEMORY     18

/* And each thing about one mode */
#define VBE_MODE_WHAT       0
#define VBE_MODE_FIRST_KIND 2
#define VBE_MODE_SECOND_KIND 3
#define VBE_MODE_GRAIN      4
#define VBE_MODE_WINDOW     6
#define VBE_MODE_FIRST_AT   8
#define VBE_MODE_SECOND_AT  10
#define VBE_MODE_MOVER      12
#define VBE_MODE_ROW_BYTES  16
#define VBE_MODE_WIDTH      18
#define VBE_MODE_HEIGHT     20
#define VBE_MODE_CELL_WIDE  22
#define VBE_MODE_CELL_TALL  23
#define VBE_MODE_PLANES     24
#define VBE_MODE_DEPTH      25
#define VBE_MODE_BANKS      26
#define VBE_MODE_ARRANGED   27
#define VBE_MODE_BANK_SIZE  28
#define VBE_MODE_PAGES      29
#define VBE_MODE_RESERVED   30
#define VBE_MODE_RED_BITS   31
#define VBE_MODE_RED_AT     32
#define VBE_MODE_GREEN_BITS 33
#define VBE_MODE_GREEN_AT   34
#define VBE_MODE_BLUE_BITS  35
#define VBE_MODE_BLUE_AT    36
#define VBE_MODE_SPARE_BITS 37
#define VBE_MODE_SPARE_AT   38
#define VBE_MODE_DIRECT     39
#define VBE_MODE_MEMORY_AT  40

/* What a mode says it is: there at all, in colour, pixels, and reachable whole */
#define VBE_IS_THERE        HEX(0001)
#define VBE_HAS_MORE        HEX(0002)
#define VBE_IN_COLOUR       HEX(0008)
#define VBE_IN_PIXELS       HEX(0010)
#define VBE_NOT_THE_OLD_WAY HEX(0020)
#define VBE_REACHED_WHOLE   HEX(0080)

/* How the pixels are arranged: a name for a colour, or a colour outright */
#define VBE_NAMED_COLOUR    4
#define VBE_OWN_COLOUR      6

/* What a caller adds to a mode number to say it wants the whole of it at once */
#define VBE_WANTS_WHOLE     HEX(4000)
#define VBE_KEEPS_SCREEN    HEX(8000)

/* Where the table this fills in ends */
#define VBE_NO_MORE_MODES   HEX(FFFF)

/* THE CALLS *****************************************************************/

/*
 * Asked which of these a display answers. Anything past the four below is
 * refused by leaving AH alone, which is what says a call is not there.
 */
Int10Vbe:
    cmp al, HEX(00)
    je VbeAboutDisplay
    cmp al, HEX(01)
    je VbeAboutMode
    cmp al, HEX(02)
    je VbeSetMode
    cmp al, HEX(03)
    je VbeWhichMode
    iret

/*
 * What the display is and what it can do, into the block at ES:DI.
 *
 * The four letters at the front are the only thing a caller is sure of before
 * it reads any of this, so they go in last: a caller that asked a display which
 * cannot answer finds whatever it put there itself, and what it puts there is
 * never these four.
 */
VbeAboutDisplay:
    push es
    push di
    push ax
    push cx
    push si

    /* Cleared first, because a caller reads fields this does not write */
    push di
    mov cx, 256
    xor al, al
    cld
VbeAboutClear:
    stosb
    loop VbeAboutClear
    pop di

    mov word ptr es:[di + VBE_INFO_VERSION], VBE_VERSION

    /* Who made it, which is only ever read back by people */
    mov word ptr es:[di + VBE_INFO_MAKER], OFFSET VbeMakerName
    mov word ptr es:[di + VBE_INFO_MAKER + 2], cs

    /* Nothing worth saying: no table of eight bit colours, nothing switched */
    mov dword ptr es:[di + VBE_INFO_CAN_DO], 0

    mov word ptr es:[di + VBE_INFO_MODES], OFFSET VbeModeNumbers
    mov word ptr es:[di + VBE_INFO_MODES + 2], cs

    mov word ptr es:[di + VBE_INFO_MEMORY], VBE_MEMORY_BLOCKS

    mov dword ptr es:[di + VBE_INFO_SIGNATURE], VBE_SIGNATURE

    pop si
    pop cx
    pop ax
    pop di
    pop es
    mov ax, VBE_SUPPORTED
    iret

/*
 * What one mode is, into the block at ES:DI. CX names the mode.
 *
 * A mode this display does not have is refused rather than described, because
 * a caller that is handed a description believes every word of it and will set
 * a mode it was told about.
 */
VbeAboutMode:
    push es
    push di
    push bx
    push cx
    push dx
    push si

    call VbeFindMode
    jc VbeAboutModeNone

    /* Cleared, because most of a mode is things this display does not do */
    push di
    push cx
    mov cx, 256
    xor al, al
    cld
VbeAboutModeClear:
    stosb
    loop VbeAboutModeClear
    pop cx
    pop di

    mov word ptr es:[di + VBE_MODE_WHAT], VBE_IS_THERE OR VBE_HAS_MORE OR VBE_IN_COLOUR OR VBE_IN_PIXELS OR VBE_NOT_THE_OLD_WAY OR VBE_REACHED_WHOLE

    /* No window onto it, because the whole of it is reachable at once */
    mov byte ptr es:[di + VBE_MODE_FIRST_KIND], 0
    mov byte ptr es:[di + VBE_MODE_SECOND_KIND], 0
    mov word ptr es:[di + VBE_MODE_GRAIN], 0
    mov word ptr es:[di + VBE_MODE_WINDOW], 0
    mov word ptr es:[di + VBE_MODE_FIRST_AT], 0
    mov word ptr es:[di + VBE_MODE_SECOND_AT], 0
    mov dword ptr es:[di + VBE_MODE_MOVER], 0

    /* How wide and how tall, straight out of the table */
    mov ax, word ptr cs:[si + VBE_TABLE_WIDTH]
    mov word ptr es:[di + VBE_MODE_WIDTH], ax
    mov ax, word ptr cs:[si + VBE_TABLE_HEIGHT]
    mov word ptr es:[di + VBE_MODE_HEIGHT], ax

    mov byte ptr es:[di + VBE_MODE_CELL_WIDE], 8
    mov byte ptr es:[di + VBE_MODE_CELL_TALL], 16
    mov byte ptr es:[di + VBE_MODE_PLANES], 1
    mov byte ptr es:[di + VBE_MODE_BANKS], 1
    mov byte ptr es:[di + VBE_MODE_BANK_SIZE], 0
    mov byte ptr es:[di + VBE_MODE_PAGES], 0
    mov byte ptr es:[di + VBE_MODE_RESERVED], 1

    mov al, byte ptr cs:[si + VBE_TABLE_DEPTH]
    mov byte ptr es:[di + VBE_MODE_DEPTH], al

    /* How long a row is, which is as many bytes as a pixel costs, times across */
    call VbeRowBytes
    mov word ptr es:[di + VBE_MODE_ROW_BYTES], ax

    /* Where the whole of it is, which the display was asked rather than told */
    call VbeMemoryAt
    mov word ptr es:[di + VBE_MODE_MEMORY_AT], ax
    mov word ptr es:[di + VBE_MODE_MEMORY_AT + 2], dx

    /* And how a pixel is made up, which is the one thing that differs by depth */
    mov al, byte ptr cs:[si + VBE_TABLE_DEPTH]
    cmp al, 8
    je VbeAboutModeNamed

    mov byte ptr es:[di + VBE_MODE_ARRANGED], VBE_OWN_COLOUR

    cmp al, 16
    je VbeAboutModeSixteen

    /* Eight bits to each of the three, and eight more nothing looks at */
    mov byte ptr es:[di + VBE_MODE_RED_BITS], 8
    mov byte ptr es:[di + VBE_MODE_RED_AT], 16
    mov byte ptr es:[di + VBE_MODE_GREEN_BITS], 8
    mov byte ptr es:[di + VBE_MODE_GREEN_AT], 8
    mov byte ptr es:[di + VBE_MODE_BLUE_BITS], 8
    mov byte ptr es:[di + VBE_MODE_BLUE_AT], 0
    mov byte ptr es:[di + VBE_MODE_SPARE_BITS], 8
    mov byte ptr es:[di + VBE_MODE_SPARE_AT], 24
    jmp VbeAboutModeDone

VbeAboutModeSixteen:
    /* Five, six and five, because green is the one the eye is best at */
    mov byte ptr es:[di + VBE_MODE_RED_BITS], 5
    mov byte ptr es:[di + VBE_MODE_RED_AT], 11
    mov byte ptr es:[di + VBE_MODE_GREEN_BITS], 6
    mov byte ptr es:[di + VBE_MODE_GREEN_AT], 5
    mov byte ptr es:[di + VBE_MODE_BLUE_BITS], 5
    mov byte ptr es:[di + VBE_MODE_BLUE_AT], 0
    jmp VbeAboutModeDone

VbeAboutModeNamed:
    mov byte ptr es:[di + VBE_MODE_ARRANGED], VBE_NAMED_COLOUR

VbeAboutModeDone:
    pop si
    pop dx
    pop cx
    pop bx
    pop di
    pop es
    mov ax, VBE_SUPPORTED
    iret

VbeAboutModeNone:
    pop si
    pop dx
    pop cx
    pop bx
    pop di
    pop es
    mov ax, HEX(014F)
    iret

/*
 * Into the mode BX names, or refused.
 *
 * The two top bits of BX are the caller's rather than the mode's: one asks for
 * the whole of the memory at once and the other asks for what is on the screen
 * to be left alone. This display has no other way of being reached, so the
 * first is taken and nothing is done about it.
 */
VbeSetMode:
    push ax
    push bx
    push cx
    push dx
    push si

    mov cx, bx
    and cx, HEX(3FFF)

    /* Going back to a screen of characters is the old call's business */
    cmp cx, HEX(0003)
    jne VbeSetModeLook

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    push ax
    mov ax, HEX(0003)
    int HEX(10)
    pop ax
    mov ax, VBE_SUPPORTED
    iret

VbeSetModeLook:
    call VbeFindMode
    jc VbeSetModeNone

    call VbeProgram

    /* Kept, because the call that asks which mode it is in has no other way */
    mov word ptr cs:[VbeCurrentMode], cx

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    mov ax, VBE_SUPPORTED
    iret

VbeSetModeNone:
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    mov ax, HEX(014F)
    iret

/* Which one it is in, which is only ever the last one it was put into */
VbeWhichMode:
    mov bx, word ptr cs:[VbeCurrentMode]
    mov ax, VBE_SUPPORTED
    iret

/* FINDING ONE ****************************************************************/

/*
 * Walks the table for the mode in CX and leaves SI pointing at it. Carry set
 * means this display does not have that one.
 */
VbeFindMode:
    push ax

    mov si, OFFSET VbeModeTable

VbeFindModeNext:
    mov ax, word ptr cs:[si + VBE_TABLE_NUMBER]
    cmp ax, VBE_NO_MORE_MODES
    je VbeFindModeNone

    cmp ax, cx
    je VbeFindModeFound

    add si, VBE_TABLE_LENGTH
    jmp VbeFindModeNext

VbeFindModeFound:
    pop ax
    clc
    ret

VbeFindModeNone:
    pop ax
    stc
    ret

/* How many bytes one row of the mode at SI is, into AX */
VbeRowBytes:
    push bx
    push dx

    mov ax, word ptr cs:[si + VBE_TABLE_WIDTH]
    movzx bx, byte ptr cs:[si + VBE_TABLE_DEPTH]
    add bx, 7
    shr bx, 3
    mul bx

    pop dx
    pop bx
    ret

/*
 * Where the whole of what is drawn out of was put, into DX:AX.
 *
 * Asked of the display rather than decided here. Two of its registers hold the
 * top half of the number, and they hold it because the display put it there
 * when it was given the memory, not because anything wrote it.
 */
VbeMemoryAt:
    push cx

    mov al, CRTC_LINEAR_HIGH
    call VbeReadCrtc
    mov dh, al

    mov al, CRTC_LINEAR_LOW
    call VbeReadCrtc
    mov dl, al

    xor ax, ax

    pop cx
    ret

/* PUTTING THE DISPLAY INTO ONE ***********************************************/

/*
 * Writes the registers that make the display be the mode at SI.
 *
 * Nothing here is a mode number: every one of these says one thing about the
 * picture, and a mode is only ever the handful of numbers that come out of
 * them together. That is why a display of this kind has so many more modes
 * than there were numbers for.
 */
VbeProgram:
    push ax
    push bx
    push cx
    push dx

    /* Unlocked, because none of the display's own registers answer until it is */
    mov al, VESA_CRTC_UNLOCK_ONE
    mov ah, HEX(48)
    call VbeWriteCrtc
    mov al, VESA_CRTC_UNLOCK_TWO
    mov ah, HEX(A5)
    call VbeWriteCrtc

    /*
     * Past what a plain display can do. Everything below is read differently
     * once this is set, so it goes first, and the two bits beside it that
     * would otherwise move the top of the picture are put back to nothing.
     */
    mov al, VESA_CRTC_MEMORY_CONFIG
    call VbeReadCrtc
    and al, HEX(CF)
    or al, HEX(08)
    mov ah, al
    mov al, VESA_CRTC_MEMORY_CONFIG
    call VbeWriteCrtc

    /* How much of a pixel there is, which is the top half of one register */
    mov ah, HEX(00)
    cmp byte ptr cs:[si + VBE_TABLE_DEPTH], 16
    jne VbeProgramNotSixteen
    mov ah, HEX(50)
VbeProgramNotSixteen:
    cmp byte ptr cs:[si + VBE_TABLE_DEPTH], 32
    jne VbeProgramDac
    mov ah, HEX(D0)
VbeProgramDac:
    mov al, VESA_CRTC_DAC_MODE
    call VbeWriteCrtc

    /*
     * How wide. The register counts the clock the picture is drawn with rather
     * than pixels, and a clock carries half as many of them when each one is
     * two bytes, so the number put in is twice what it looks like it should be.
     */
    mov ax, word ptr cs:[si + VBE_TABLE_WIDTH]
    cmp byte ptr cs:[si + VBE_TABLE_DEPTH], 16
    je VbeProgramWideHalf
    shr ax, 3
    jmp VbeProgramWide
VbeProgramWideHalf:
    shr ax, 2
VbeProgramWide:
    dec ax
    mov bx, ax
    mov ah, al
    mov al, VESA_CRTC_WIDTH_END
    call VbeWriteCrtc

    mov al, VESA_CRTC_EXTENDED_WIDTH
    call VbeReadCrtc
    and al, HEX(FD)
    test bh, 1
    jz VbeProgramWideDone
    or al, HEX(02)
VbeProgramWideDone:
    mov ah, al
    mov al, VESA_CRTC_EXTENDED_WIDTH
    call VbeWriteCrtc

    /* How tall, whose top three bits are kept in two other registers */
    mov ax, word ptr cs:[si + VBE_TABLE_HEIGHT]
    dec ax
    mov bx, ax
    mov ah, al
    mov al, VESA_CRTC_HEIGHT_END
    call VbeWriteCrtc

    mov al, VESA_CRTC_OVERFLOW
    call VbeReadCrtc
    and al, HEX(BD)
    test bh, 1
    jz VbeProgramTallOne
    or al, HEX(02)
VbeProgramTallOne:
    test bh, 2
    jz VbeProgramTallTwo
    or al, HEX(40)
VbeProgramTallTwo:
    /* And the picture stops where it ends rather than somewhere above it */
    or al, HEX(08)
    mov ah, al
    mov al, VESA_CRTC_OVERFLOW
    call VbeWriteCrtc

    mov al, VESA_CRTC_EXTENDED_HEIGHT
    call VbeReadCrtc
    and al, HEX(F9)
    test bh, 4
    jz VbeProgramTallThree
    or al, HEX(02)
VbeProgramTallThree:
    or al, HEX(04)
    mov ah, al
    mov al, VESA_CRTC_EXTENDED_HEIGHT
    call VbeWriteCrtc

    mov al, VESA_CRTC_BLANK_START
    mov ah, HEX(FF)
    call VbeWriteCrtc

    /* Every row drawn once, and the rest of where the picture stops */
    mov al, VESA_CRTC_MAX_ROW
    call VbeReadCrtc
    and al, HEX(7F)
    or al, HEX(20)
    mov ah, al
    mov al, VESA_CRTC_MAX_ROW
    call VbeWriteCrtc

    /*
     * How far apart two rows are, counted in pairs of pixels, so half the
     * width. Two bits of it do not fit and are kept beside the pitch.
     */
    mov ax, word ptr cs:[si + VBE_TABLE_WIDTH]
    shr ax, 1
    mov bx, ax
    mov ah, al
    mov al, VESA_CRTC_ROW_LENGTH
    call VbeWriteCrtc

    mov al, VESA_CRTC_EXTENDED_SYSTEM
    call VbeReadCrtc
    and al, HEX(CC)
    mov cl, bh
    and cl, HEX(03)
    shl cl, 4
    or al, cl
    mov ah, al
    mov al, VESA_CRTC_EXTENDED_SYSTEM
    call VbeWriteCrtc

    /* The first pixel of the picture is the first one in the memory */
    mov al, VESA_CRTC_START_HIGH
    xor ah, ah
    call VbeWriteCrtc
    mov al, VESA_CRTC_START_LOW
    xor ah, ah
    call VbeWriteCrtc
    mov al, VESA_CRTC_EXTENDED_START
    xor ah, ah
    call VbeWriteCrtc

    /* Drawn in one pass rather than two, and no half pitch added to it */
    mov al, VESA_CRTC_MODE_CONTROL
    call VbeReadCrtc
    and al, HEX(DF)
    mov ah, al
    mov al, VESA_CRTC_MODE_CONTROL
    call VbeWriteCrtc

    mov al, VESA_CRTC_EXTENDED_MODE
    call VbeReadCrtc
    and al, HEX(FB)
    mov ah, al
    mov al, VESA_CRTC_EXTENDED_MODE
    call VbeWriteCrtc

    /* The whole of the memory reachable at once, all four megabytes of it */
    mov al, VESA_CRTC_LINEAR_CONTROL
    mov ah, HEX(13)
    call VbeWriteCrtc

    /* The clock the picture is drawn with, not halved */
    mov dx, VESA_SEQ_ADDRESS
    mov al, 1
    out dx, al
    inc dx
    in al, dx
    and al, HEX(F7)
    out dx, al

    pop dx
    pop cx
    pop bx
    pop ax
    ret

/* The value of the register AL names, into AL */
VbeReadCrtc:
    push dx
    mov dx, VESA_CRTC_ADDRESS
    out dx, al
    inc dx
    in al, dx
    pop dx
    ret

/* And AH into the register AL names */
VbeWriteCrtc:
    push ax
    push dx
    mov dx, VESA_CRTC_ADDRESS
    out dx, al
    inc dx
    mov al, ah
    out dx, al
    pop dx
    pop ax
    ret

/* WHAT THERE IS **************************************************************/

/* Which one it was last put into, which is a screen of characters to begin with */
VbeCurrentMode:
    .word HEX(0003)

VbeMakerName:
    .asciz "ReacTVmm"

/*
 * Every mode this display has, as a number, how wide, how tall and how much of
 * a pixel. The numbers are the ones everything has agreed on for these shapes,
 * because a caller looking for one of them looks for the number rather than
 * for the shape.
 */
VbeModeTable:
    .word HEX(0101)
    .word 640
    .word 480
    .byte 8
    .byte 0

    .word HEX(0103)
    .word 800
    .word 600
    .byte 8
    .byte 0

    .word HEX(0105)
    .word 1024
    .word 768
    .byte 8
    .byte 0

    .word HEX(0111)
    .word 640
    .word 480
    .byte 16
    .byte 0

    .word HEX(0114)
    .word 800
    .word 600
    .byte 16
    .byte 0

    .word HEX(0117)
    .word 1024
    .word 768
    .byte 16
    .byte 0

    .word HEX(0112)
    .word 640
    .word 480
    .byte 32
    .byte 0

    .word HEX(0115)
    .word 800
    .word 600
    .byte 32
    .byte 0

    .word HEX(0118)
    .word 1024
    .word 768
    .byte 32
    .byte 0

    .word VBE_NO_MORE_MODES
    .word 0
    .word 0
    .byte 0
    .byte 0

/* The same numbers again on their own, which is what a caller is handed */
VbeModeNumbers:
    .word HEX(0101), HEX(0103), HEX(0105)
    .word HEX(0111), HEX(0114), HEX(0117)
    .word HEX(0112), HEX(0115), HEX(0118)
    .word VBE_NO_MORE_MODES
