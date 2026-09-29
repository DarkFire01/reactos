/*
 * Putting the display into a mode.
 *
 * A mode is nothing but a list of numbers written into the display's own
 * registers: how wide a line is, how many of them, where the memory it reads
 * sits, and whether a byte there is a character or eight pixels. Recording
 * which mode was asked for and clearing the page is not setting one. Anything
 * that asks for a mode and then writes pixels is entitled to find the display
 * arranged for pixels, and a display still arranged for characters throws
 * every one of those writes away.
 */

/* Where each file of registers is reached */
#define VGA_ATTRIBUTE       HEX(03C0)
#define VGA_ATTRIBUTE_READ  HEX(03C1)
#define VGA_MISC_WRITE      HEX(03C2)
#define VGA_SEQUENCER       HEX(03C4)
#define VGA_SEQUENCER_DATA  HEX(03C5)
#define VGA_COLOUR_WRITE    HEX(03C8)
#define VGA_COLOUR_DATA     HEX(03C9)
#define VGA_GRAPHICS        HEX(03CE)
#define VGA_GRAPHICS_DATA   HEX(03CF)
#define VGA_CRTC            HEX(03D4)
#define VGA_CRTC_DATA       HEX(03D5)
#define VGA_STATUS_READ     HEX(03DA)

/* How many of each a mode describes */
#define VGA_SEQUENCER_COUNT 5
#define VGA_CRTC_COUNT      25
#define VGA_GRAPHICS_COUNT  9
#define VGA_ATTRIBUTE_COUNT 21

/* The register that keeps the first few of the shape from being changed */
#define VGA_CRTC_PROTECT    HEX(11)

/* Written to the one file reached through a single port to let the beam out */
#define VGA_ATTRIBUTE_ON    HEX(20)

/* SETTING ONE ****************************************************************/

/*
 * Writes one whole file of registers, naming each in turn.
 *
 * DS:SI is the list, CX how many, DX where they are named and DX+1 where they
 * are written. The first of them is numbered zero and they run on from there,
 * which is true of every file here but the last.
 */
VgaWriteFile:
    push ax
    push cx
    push dx
    push si

    xor ah, ah

VgaWriteNext:
    mov al, ah
    out dx, al

    inc dx
    lodsb
    out dx, al
    dec dx

    inc ah
    loop VgaWriteNext

    pop si
    pop dx
    pop cx
    pop ax
    ret

/*
 * The one file that is named and written through the same port, and which
 * has to be told to let the beam out again afterwards.
 */
VgaWriteAttributes:
    push ax
    push cx
    push dx
    push si

    /* Whichever half it was expecting, it is expecting a name after this */
    mov dx, VGA_STATUS_READ
    in al, dx

    xor ah, ah
    mov dx, VGA_ATTRIBUTE

VgaWriteAttributeNext:
    mov al, ah
    out dx, al
    lodsb
    out dx, al

    inc ah
    loop VgaWriteAttributeNext

    /* Nothing is shown until this is written, whatever the rest of it says */
    mov al, VGA_ATTRIBUTE_ON
    out dx, al

    pop si
    pop dx
    pop cx
    pop ax
    ret

/*
 * Puts the display into the mode in AL. Carry set means it is a mode this
 * does not have the numbers for, and nothing was changed.
 */
VgaSetMode:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    push cs
    pop ds

    cmp al, HEX(12)
    je VgaSetPixels
    cmp al, HEX(03)
    je VgaSetCharacters
    cmp al, HEX(02)
    je VgaSetCharacters
    cmp al, HEX(07)
    je VgaSetCharacters

    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    stc
    ret

VgaSetCharacters:
    mov si, OFFSET VgaTextRegisters
    jmp VgaApply

VgaSetPixels:
    mov si, OFFSET VgaPixelRegisters

/*
 * Every file in the order the part wants them: the one that says where the
 * whole thing answers first, because the rest are named relative to it.
 */
VgaApply:
    /* Nothing is drawn while it is half arranged */
    mov dx, VGA_SEQUENCER
    xor al, al
    out dx, al
    inc dx
    mov al, HEX(01)
    out dx, al

    mov dx, VGA_MISC_WRITE
    lodsb
    out dx, al

    mov dx, VGA_SEQUENCER
    mov cx, VGA_SEQUENCER_COUNT
    call VgaWriteFile
    add si, VGA_SEQUENCER_COUNT

    /*
     * The first few of the shape are held against being written, and the
     * register holding them is one of the ones about to be written.
     */
    mov dx, VGA_CRTC
    mov al, VGA_CRTC_PROTECT
    out dx, al
    inc dx
    in al, dx
    and al, HEX(7F)
    out dx, al

    mov dx, VGA_CRTC
    mov cx, VGA_CRTC_COUNT
    call VgaWriteFile
    add si, VGA_CRTC_COUNT

    mov dx, VGA_GRAPHICS
    mov cx, VGA_GRAPHICS_COUNT
    call VgaWriteFile
    add si, VGA_GRAPHICS_COUNT

    mov cx, VGA_ATTRIBUTE_COUNT
    call VgaWriteAttributes
    add si, VGA_ATTRIBUTE_COUNT

    /* And let it run again */
    mov dx, VGA_SEQUENCER
    xor al, al
    out dx, al
    inc dx
    mov al, HEX(03)
    out dx, al

    call VgaSetColours

    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    clc
    ret

/*
 * The sixteen a display of this kind comes up with. Each part is six bits, so
 * the largest is not two hundred and fifty five but sixty three.
 */
VgaSetColours:
    push ax
    push cx
    push dx
    push si
    push ds

    push cs
    pop ds

    mov dx, VGA_COLOUR_WRITE
    xor al, al
    out dx, al

    mov dx, VGA_COLOUR_DATA
    mov si, OFFSET VgaColours
    mov cx, 16 * 3
    cld
    rep outsb

    pop ds
    pop si
    pop dx
    pop cx
    pop ax
    ret

/*
 * Empties what a screen made of pixels is drawn out of. All four stores at
 * once, because a pixel has a bit in each and clearing one of them would
 * leave the screen holding three quarters of whatever was there.
 */
VgaClearPlanes:
    push ax
    push cx
    push dx
    push di
    push es

    /* Every store written by one write, and the write taken as it is given */
    mov dx, VGA_SEQUENCER
    mov al, HEX(02)
    out dx, al
    inc dx
    mov al, HEX(0F)
    out dx, al

    mov dx, VGA_GRAPHICS
    mov al, HEX(05)
    out dx, al
    inc dx
    xor al, al
    out dx, al

    mov dx, VGA_GRAPHICS
    mov al, HEX(08)
    out dx, al
    inc dx
    mov al, HEX(FF)
    out dx, al

    mov ax, HEX(A000)
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, HEX(4000)
    cld
    rep stosw

    pop es
    pop di
    pop dx
    pop cx
    pop ax
    ret

/* WHAT EACH MODE IS **********************************************************/

/*
 * Eighty by twenty five characters, which is what a machine of this kind
 * comes up in and what everything falls back to.
 */
VgaTextRegisters:
    /* Where the whole thing answers, and how fast */
    .byte HEX(67)

    /* How the memory behind it is divided */
    .byte HEX(03), HEX(00), HEX(03), HEX(00), HEX(02)

    /* The shape of a line and of the screen */
    .byte HEX(5F), HEX(4F), HEX(50), HEX(82), HEX(55), HEX(81), HEX(BF), HEX(1F)
    .byte HEX(00), HEX(4F), HEX(0D), HEX(0E), HEX(00), HEX(00), HEX(00), HEX(00)
    .byte HEX(9C), HEX(8E), HEX(8F), HEX(28), HEX(1F), HEX(96), HEX(B9), HEX(A3)
    .byte HEX(FF)

    /* What a byte in that memory means, and where it is reached */
    .byte HEX(00), HEX(00), HEX(00), HEX(00), HEX(00), HEX(10), HEX(0E), HEX(00)
    .byte HEX(FF)

    /* Which of the last table's colours each of the sixteen stands for */
    .byte HEX(00), HEX(01), HEX(02), HEX(03), HEX(04), HEX(05), HEX(14), HEX(07)
    .byte HEX(38), HEX(39), HEX(3A), HEX(3B), HEX(3C), HEX(3D), HEX(3E), HEX(3F)
    .byte HEX(0C), HEX(00), HEX(0F), HEX(08), HEX(00)

/*
 * Six hundred and forty by four hundred and eighty, sixteen colours, which is
 * the one mode every system of this age asks for before it draws anything.
 */
VgaPixelRegisters:
    .byte HEX(E3)

    .byte HEX(03), HEX(01), HEX(0F), HEX(00), HEX(06)

    .byte HEX(5F), HEX(4F), HEX(50), HEX(82), HEX(54), HEX(80), HEX(0B), HEX(3E)
    .byte HEX(00), HEX(40), HEX(00), HEX(00), HEX(00), HEX(00), HEX(00), HEX(00)
    .byte HEX(EA), HEX(8C), HEX(DF), HEX(28), HEX(00), HEX(E7), HEX(04), HEX(E3)
    .byte HEX(FF)

    /* The one that says pixels rather than characters, and where they sit */
    .byte HEX(00), HEX(00), HEX(00), HEX(00), HEX(03), HEX(00), HEX(05), HEX(0F)
    .byte HEX(FF)

    /*
     * Each of the sixteen naming the colour of the same number, rather than
     * the scattered set a screen of characters uses. Anything drawing pixels
     * fills in the first sixteen colours and then draws with the numbers
     * nought to fifteen; sending those off to the fifties leaves it drawing
     * with colours it never set.
     */
    .byte HEX(00), HEX(01), HEX(02), HEX(03), HEX(04), HEX(05), HEX(06), HEX(07)
    .byte HEX(08), HEX(09), HEX(0A), HEX(0B), HEX(0C), HEX(0D), HEX(0E), HEX(0F)
    .byte HEX(01), HEX(00), HEX(0F), HEX(00), HEX(00)

/* The sixteen, as three parts of six bits each */
VgaColours:
    .byte HEX(00), HEX(00), HEX(00)
    .byte HEX(00), HEX(00), HEX(2A)
    .byte HEX(00), HEX(2A), HEX(00)
    .byte HEX(00), HEX(2A), HEX(2A)
    .byte HEX(2A), HEX(00), HEX(00)
    .byte HEX(2A), HEX(00), HEX(2A)
    .byte HEX(2A), HEX(15), HEX(00)
    .byte HEX(2A), HEX(2A), HEX(2A)
    .byte HEX(15), HEX(15), HEX(15)
    .byte HEX(15), HEX(15), HEX(3F)
    .byte HEX(15), HEX(3F), HEX(15)
    .byte HEX(15), HEX(3F), HEX(3F)
    .byte HEX(3F), HEX(15), HEX(15)
    .byte HEX(3F), HEX(15), HEX(3F)
    .byte HEX(3F), HEX(3F), HEX(15)
    .byte HEX(3F), HEX(3F), HEX(3F)
