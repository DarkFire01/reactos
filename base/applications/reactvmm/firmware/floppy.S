/*
 * The removable drive, as the firmware drives it.
 *
 * Sectors do not come back through a register here. The controller is told
 * where to put them and one of the transfer channels carries them, so the work
 * is in programming that channel correctly and the read itself is four writes
 * and a wait.
 */

#define FDC_DIGITAL_OUTPUT  HEX(03F2)
#define FDC_MAIN_STATUS     HEX(03F4)
#define FDC_DATA            HEX(03F5)
#define FDC_CONFIGURATION   HEX(03F7)

/* What the main status register says when it wants a byte, or has one */
#define FDC_READY           HEX(80)
#define FDC_TO_PROCESSOR    HEX(40)
#define FDC_EITHER_WAY      HEX(C0)

/* Drive zero selected, out of reset, channel enabled, its motor turning */
#define FDC_RUNNING         HEX(1C)
#define FDC_STOPPED         HEX(0C)

/* The channel the drive is wired to, and the registers that program it */
#define DMA_MASK_ONE        HEX(000A)
#define DMA_MODE            HEX(000B)
#define DMA_CLEAR_FLIPFLOP  HEX(000C)
#define DMA_CHANNEL_ADDRESS HEX(0004)
#define DMA_CHANNEL_COUNT   HEX(0005)
#define DMA_CHANNEL_PAGE    HEX(0081)

/* Channel two, masked and unmasked */
#define DMA_MASK_CHANNEL    HEX(06)
#define DMA_UNMASK_CHANNEL  HEX(02)

/* One transfer at a time, filling memory, on channel two */
#define DMA_MODE_READ       HEX(46)
#define DMA_MODE_WRITE      HEX(4A)

/* WAITING FOR IT *************************************************************/

/*
 * Waits until the controller will take a byte. Carry set means it never would,
 * which is what a drive that is not there looks like.
 */
FloppyWaitWrite:
    push ax
    push cx
    push dx

    mov cx, HEX(FFFF)
    mov dx, FDC_MAIN_STATUS

FloppyWaitWriteLoop:
    in al, dx
    and al, FDC_EITHER_WAY
    cmp al, FDC_READY
    je FloppyWaitWriteReady
    loop FloppyWaitWriteLoop

    pop dx
    pop cx
    pop ax
    stc
    ret

FloppyWaitWriteReady:
    pop dx
    pop cx
    pop ax
    clc
    ret

/* The same for a byte coming back the other way */
FloppyWaitRead:
    push ax
    push cx
    push dx

    mov cx, HEX(FFFF)
    mov dx, FDC_MAIN_STATUS

FloppyWaitReadLoop:
    in al, dx
    and al, FDC_EITHER_WAY
    cmp al, FDC_EITHER_WAY
    je FloppyWaitReadReady
    loop FloppyWaitReadLoop

    pop dx
    pop cx
    pop ax
    stc
    ret

FloppyWaitReadReady:
    pop dx
    pop cx
    pop ax
    clc
    ret

/* Hands AL to the controller */
FloppyPut:
    push dx
    call FloppyWaitWrite
    jc FloppyPutDone
    mov dx, FDC_DATA
    out dx, al
FloppyPutDone:
    pop dx
    ret

/* Takes a byte back into AL */
FloppyGet:
    push dx
    call FloppyWaitRead
    jc FloppyGetDone
    mov dx, FDC_DATA
    in al, dx
FloppyGetDone:
    pop dx
    ret

/* Reads the seven bytes a finished command leaves behind, into the tail below */
FloppyResults:
    push ax
    push cx
    push di

    mov di, OFFSET FloppyResult
    mov cx, 7

FloppyResultsLoop:
    call FloppyGet
    jc FloppyResultsDone
    mov byte ptr cs:[di], al
    inc di
    loop FloppyResultsLoop

FloppyResultsDone:
    pop di
    pop cx
    pop ax
    ret

/* THE CHANNEL ****************************************************************/

/*
 * Points the channel at ES:BX for CX bytes, the way AL says.
 *
 * The address the channel wants is a flat one, so the segment and the offset
 * are added up first. The top byte of that goes in a register of its own,
 * because the chip itself was only ever given sixteen bits of address.
 */
FloppyProgramChannel:
    push ax
    push bx
    push cx
    push dx
    push si

    mov si, ax

    /* Where in memory, as one number rather than two */
    mov ax, es
    mov dx, ax
    shr dx, 12
    shl ax, 4
    add ax, bx
    adc dx, 0

    /* Nothing may move while the channel is being pointed somewhere else */
    push ax
    mov al, DMA_MASK_CHANNEL
    out DMA_MASK_ONE, al
    pop ax

    push ax
    xor al, al
    out DMA_CLEAR_FLIPFLOP, al
    pop ax

    out DMA_CHANNEL_ADDRESS, al
    mov al, ah
    out DMA_CHANNEL_ADDRESS, al

    mov al, dl
    out DMA_CHANNEL_PAGE, al

    xor al, al
    out DMA_CLEAR_FLIPFLOP, al

    /* The count is one less than the number of bytes, as the chip was made */
    mov ax, cx
    dec ax
    out DMA_CHANNEL_COUNT, al
    mov al, ah
    out DMA_CHANNEL_COUNT, al

    mov ax, si
    out DMA_MODE, al

    mov al, DMA_UNMASK_CHANNEL
    out DMA_MASK_ONE, al

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

/* BRINGING IT UP *************************************************************/

/*
 * Starts the motor and puts the head back at the outside.
 *
 * A drive whose head is somewhere unknown answers a seek to where it already
 * is by doing nothing, so the first thing asked of one has to be the one
 * command that does not care where it was.
 */
FloppyStart:
    push ax
    push cx
    push dx

    mov dx, FDC_DIGITAL_OUTPUT
    mov al, FDC_RUNNING
    out dx, al

    /* Long enough for a real one to be at speed, and harmless to one that is */
    mov cx, HEX(4000)
FloppySpinUp:
    loop FloppySpinUp

    /* The rate the medium is written at, which is the only one answered for */
    mov dx, FDC_CONFIGURATION
    xor al, al
    out dx, al

    mov al, HEX(07)
    call FloppyPut
    jc FloppyStartFailed
    xor al, al
    call FloppyPut
    jc FloppyStartFailed

    call FloppyWaitDone

    /* And what came of it, which has to be taken or the next command is refused */
    mov al, HEX(08)
    call FloppyPut
    call FloppyGet
    call FloppyGet

    pop dx
    pop cx
    pop ax
    clc
    ret

FloppyStartFailed:
    pop dx
    pop cx
    pop ax
    stc
    ret

FloppyStop:
    push ax
    push dx

    mov dx, FDC_DIGITAL_OUTPUT
    mov al, FDC_STOPPED
    out dx, al

    pop dx
    pop ax
    ret

/*
 * Waits for a command that answers with a line rather than with bytes.
 *
 * The line is watched rather than the register, because a seek leaves nothing
 * in the register to watch and there is no other way to know it finished.
 */
FloppyWaitDone:
    push ax
    push cx
    push ds

    mov ax, BDA_SEGMENT
    mov ds, ax
    mov byte ptr ds:[BDA_FLOPPY_DONE], 0

    mov cx, HEX(FFFF)
    sti

FloppyWaitDoneLoop:
    cmp byte ptr ds:[BDA_FLOPPY_DONE], 0
    jne FloppyWaitDoneReady
    loop FloppyWaitDoneLoop

FloppyWaitDoneReady:
    pop ds
    pop cx
    pop ax
    ret

/* READING ONE ****************************************************************/

/*
 * Reads AL sectors at CH/DH/CL into ES:BX, the way the disk call asks for it.
 *
 * Carry set means the drive would not, and the first of the bytes it left
 * behind says why in the terms the caller already understands.
 */
FloppyReadSectors:
    push ax
    push bx
    push cx
    push dx
    push si

    /* How many sectors were asked for, kept where nothing else needs to go */
    movzx si, al

    /*
     * The channel wants a length in bytes and it wants it in CX, which is
     * also where the place on the medium is. The place is put back straight
     * after, because everything below reads it out of CH and CL.
     */
    push cx
    mov cx, si
    shl cx, 9
    mov al, DMA_MODE_READ
    call FloppyProgramChannel
    pop cx

    /* Both heads, both densities, and read */
    mov al, HEX(E6)
    call FloppyPut

    /* Which head and which drive, as one byte */
    mov al, dh
    and al, 1
    shl al, 2
    call FloppyPut

    /* Where on the medium */
    mov al, ch
    call FloppyPut
    mov al, dh
    and al, 1
    call FloppyPut
    mov al, cl
    and al, HEX(3F)
    call FloppyPut

    /* Sectors of the only size anything uses */
    mov al, 2
    call FloppyPut

    /* As far as the end of the track, which the controller stops at anyway */
    mov al, byte ptr cs:[FloppySectorsPerTrack]
    call FloppyPut

    mov al, HEX(1B)
    call FloppyPut
    mov al, HEX(FF)
    call FloppyPut

    call FloppyResults

    /* The first of them says whether it went, in its top two bits */
    mov al, byte ptr cs:[FloppyResult]
    and al, HEX(C0)
    jnz FloppyReadFailed

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    clc
    ret

FloppyReadFailed:
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    stc
    ret

FloppySectorsPerTrack:
    .byte 18

FloppyResult:
    .byte 0, 0, 0, 0, 0, 0, 0
