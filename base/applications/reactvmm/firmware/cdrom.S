/*
 * Booting from an optical drive.
 *
 * A drive of this kind is not told what to do by writing registers. It is
 * handed a whole command of twelve bytes and answers with however much it has,
 * which it says in the two registers that carry the middle of an address on a
 * drive that has one.
 *
 * What makes a disc bootable is three sectors in a chain: one at a fixed place
 * says where a catalogue is, the catalogue says where an image is, and the
 * image is what a machine of this kind has always loaded to and jumped into.
 */

#define CD_BASE             HEX(0170)
#define CD_DATA             HEX(0170)
#define CD_ERROR            HEX(0171)
#define CD_COUNT            HEX(0172)
#define CD_LBA_LOW          HEX(0173)
#define CD_LBA_MID          HEX(0174)
#define CD_LBA_HIGH         HEX(0175)
#define CD_SELECT           HEX(0176)
#define CD_STATUS           HEX(0177)
#define CD_COMMAND          HEX(0177)
#define CD_CONTROL          HEX(0376)

/* What the status says */
#define CD_STATUS_ERROR     HEX(01)
#define CD_STATUS_DATA      HEX(08)
#define CD_STATUS_BUSY      HEX(80)

/* The two commands a drive of this kind is given through a register */
#define CD_IDENTIFY         HEX(A1)
#define CD_PACKET           HEX(A0)

/* What it says it is when asked the ordinary way and refuses */
#define CD_SIGNATURE_MID    HEX(14)
#define CD_SIGNATURE_HIGH   HEX(EB)

/* Where the thing that says a disc is bootable always sits */
#define CD_BOOT_RECORD      17

/* How much one of its sectors holds */
#define CD_SECTOR_SIZE      2048

/* Somewhere to put one while the chain is being followed */
#define CD_SCRATCH_SEGMENT  HEX(2000)

/* WAITING ********************************************************************/

/* Waits for the drive to stop being busy. Carry set means it never did */
CdWaitReady:
    push ax
    push cx
    push dx

    mov cx, HEX(FFFF)
    mov dx, CD_STATUS

CdWaitReadyLoop:
    in al, dx
    test al, CD_STATUS_BUSY
    jz CdWaitReadyDone
    loop CdWaitReadyLoop

    pop dx
    pop cx
    pop ax
    stc
    ret

CdWaitReadyDone:
    pop dx
    pop cx
    pop ax
    clc
    ret

/* And for it to have something to hand over */
CdWaitData:
    push ax
    push cx
    push dx

    mov cx, HEX(FFFF)
    mov dx, CD_STATUS

CdWaitDataLoop:
    in al, dx
    test al, CD_STATUS_ERROR
    jnz CdWaitDataFailed
    test al, CD_STATUS_BUSY
    jnz CdWaitDataNext
    test al, CD_STATUS_DATA
    jnz CdWaitDataReady

CdWaitDataNext:
    loop CdWaitDataLoop

CdWaitDataFailed:
    pop dx
    pop cx
    pop ax
    stc
    ret

CdWaitDataReady:
    pop dx
    pop cx
    pop ax
    clc
    ret

/* FINDING ONE ****************************************************************/

/*
 * Whether there is a drive of this kind on the second pair of addresses.
 *
 * Asked the way an ordinary drive is asked, which one of these refuses while
 * leaving two registers holding what it really is. That refusal is the answer.
 */
CdPresent:
    push ax
    push dx

    mov dx, CD_SELECT
    mov al, HEX(A0)
    out dx, al

    call CdWaitReady
    jc CdPresentNo

    /* Asked what it is, in the way that only an ordinary drive answers */
    mov dx, CD_COMMAND
    mov al, HEX(EC)
    out dx, al

    call CdWaitReady

    mov dx, CD_LBA_MID
    in al, dx
    cmp al, CD_SIGNATURE_MID
    jne CdPresentNo

    mov dx, CD_LBA_HIGH
    in al, dx
    cmp al, CD_SIGNATURE_HIGH
    jne CdPresentNo

    pop dx
    pop ax
    clc
    ret

CdPresentNo:
    pop dx
    pop ax
    stc
    ret

/* READING ONE ****************************************************************/

/*
 * Reads one sector, the one numbered in CdSector, into ES:BX.
 *
 * The command is built here rather than by the caller because every field of
 * it but the number is the same every time.
 */
CdReadSector:
    push ax
    push cx
    push dx
    push di
    push si
    push ds

    mov dx, CD_SELECT
    mov al, HEX(A0)
    out dx, al

    call CdWaitReady
    jc CdReadFailed

    /* How much this is willing to take back, which is one whole sector */
    mov dx, CD_LBA_MID
    mov al, CD_SECTOR_SIZE AND HEX(FF)
    out dx, al
    mov dx, CD_LBA_HIGH
    mov al, CD_SECTOR_SIZE SHR 8
    out dx, al

    mov dx, CD_COMMAND
    mov al, CD_PACKET
    out dx, al

    call CdWaitData
    jc CdReadFailed

    /* The command itself, six words through the one register */
    push cs
    pop ds
    mov si, OFFSET CdCommand
    mov dx, CD_DATA
    mov cx, 6
    cld
    rep outsw

    call CdWaitData
    jc CdReadFailed

    /* And the sector back, a word at a time */
    mov dx, CD_DATA
    mov di, bx
    mov cx, CD_SECTOR_SIZE / 2
    cld
    rep insw

    pop ds
    pop si
    pop di
    pop dx
    pop cx
    pop ax
    clc
    ret

CdReadFailed:
    pop ds
    pop si
    pop di
    pop dx
    pop cx
    pop ax
    stc
    ret

/* HOW MUCH IS THERE **********************************************************/

/*
 * Asks the drive how much the disc holds, which comes back as the number of
 * the last sector and how big one of them is. Both arrive the opposite way
 * round from how anything here holds a number, so only the first is kept and
 * only after being turned round.
 *
 * Carry set means it would not say, and the count is left as it was.
 */
CdCapacity:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    mov dx, CD_SELECT
    mov al, HEX(A0)
    out dx, al

    call CdWaitReady
    jc CdCapacityFailed

    /* Eight bytes is the whole of what it has to say */
    mov dx, CD_LBA_MID
    mov al, 8
    out dx, al
    mov dx, CD_LBA_HIGH
    xor al, al
    out dx, al

    mov dx, CD_COMMAND
    mov al, CD_PACKET
    out dx, al

    call CdWaitData
    jc CdCapacityFailed

    push cs
    pop ds
    mov si, OFFSET CdCapacityCommand
    mov dx, CD_DATA
    mov cx, 6
    cld
    rep outsw

    call CdWaitData
    jc CdCapacityFailed

    push cs
    pop es
    mov di, OFFSET CdCapacityAnswer
    mov dx, CD_DATA
    mov cx, 4
    cld
    rep insw

    /* The last one turned round, and one past it is how many there are */
    push cs
    pop ds
    mov bx, OFFSET CdCapacityAnswer
    mov ah, byte ptr ds:[bx]
    mov al, byte ptr ds:[bx + 1]
    mov dx, ax
    mov ah, byte ptr ds:[bx + 2]
    mov al, byte ptr ds:[bx + 3]
    add ax, 1
    adc dx, 0

    mov word ptr cs:[CdSectorCount], ax
    mov word ptr cs:[CdSectorCount + 2], dx

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

CdCapacityFailed:
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

/*
 * How the drive describes itself. For one of these that is the size of a
 * sector and how many there are: the rest of what the block holds describes a
 * shape a disc does not have, and is left at nothing so that nothing tries to
 * address one by it.
 */
Int13CdParameters:
    push ax
    push bx
    push cx
    push dx
    push di
    push es

    call CdCapacity

    push ds
    pop es
    mov di, si

    /* Only the part every version of this block has, so nothing reads past it */
    mov word ptr es:[di], HEX(001A)
    mov word ptr es:[di + 2], HEX(0004)

    mov dword ptr es:[di + 4], 0
    mov dword ptr es:[di + 8], 0
    mov dword ptr es:[di + 12], 0

    mov eax, dword ptr cs:[CdSectorCount]
    mov dword ptr es:[di + 16], eax
    mov dword ptr es:[di + 20], 0

    mov word ptr es:[di + 24], CD_SECTOR_SIZE

    pop es
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    xor ah, ah
    jmp Int13Succeeded

/*
 * Puts the number in DX:AX into the command, the way round the command wants
 * it, which is the opposite of the way everything here holds one. It is kept
 * as well, because the only way to read the next sector is to count on from
 * this one and a number this big does not survive being counted in the
 * command itself.
 */
CdSetSector:
    push bx

    mov word ptr cs:[CdSector], ax
    mov word ptr cs:[CdSector + 2], dx

    mov bx, OFFSET CdCommand
    mov byte ptr cs:[bx + 2], dh
    mov byte ptr cs:[bx + 3], dl
    mov byte ptr cs:[bx + 4], ah
    mov byte ptr cs:[bx + 5], al

    pop bx
    ret

/*
 * On to the one after it, all four bytes of it.
 *
 * Counting only the low half is enough for a disc of a hundred and thirty
 * megabytes and no more, and everything past that reads as something near the
 * front instead. A disc holds far more than that.
 */
CdNextSector:
    push ax
    push dx

    mov ax, word ptr cs:[CdSector]
    mov dx, word ptr cs:[CdSector + 2]
    add ax, 1
    adc dx, 0
    call CdSetSector

    pop dx
    pop ax
    ret

/* BOOTING FROM ONE ***********************************************************/

/*
 * Follows the chain and loads what it ends at.
 *
 * Carry set means the disc is not one that can be booted, which is not a
 * failure worth stopping for: the fixed disk is tried next.
 */
CdBoot:
    push bx
    push cx
    push dx
    push es

    call CdPresent
    jc CdBootNone

    push cs
    pop ds
    mov si, OFFSET CdFoundMessage
    call SerialWriteString

    /* The sector that says whether there is a catalogue, and where */
    mov ax, CD_BOOT_RECORD
    xor dx, dx
    call CdSetSector

    mov ax, CD_SCRATCH_SEGMENT
    mov es, ax
    xor bx, bx
    call CdReadSector
    jc CdBootNone

    /* It has to say it is one of these, and say the right name */
    cmp byte ptr es:[0], 0
    jne CdBootNone
    cmp byte ptr es:[7], HEX(45)
    jne CdBootNone

    /* Where the catalogue is, which is four bytes well inside the sector */
    mov ax, word ptr es:[71]
    mov dx, word ptr es:[73]
    call CdSetSector

    xor bx, bx
    call CdReadSector
    jc CdBootNone

    /* The first entry past the one that only says the catalogue is real */
    cmp byte ptr es:[32], HEX(88)
    jne CdBootNone

    /*
     * What the disc wants the machine to think it is booting from. A disc that
     * says nothing is booted as itself; one that names a removable drive is
     * pretending to be one, and what was written for that drive is entitled to
     * find it where such a drive has always been rather than on a disc.
     */
    mov al, byte ptr es:[33]
    mov byte ptr cs:[CdPretending], al

    /* How many halves of a sector to load, and where they start */
    mov cx, word ptr es:[38]
    mov ax, word ptr es:[40]
    mov dx, word ptr es:[42]

    /* Kept whole, because everything the drive is asked for counts from it */
    mov word ptr cs:[CdPretendAt], ax
    mov word ptr cs:[CdPretendAt + 2], dx

    /* Kept, because whatever is loaded asks for both of them afterwards */
    mov word ptr cs:[CdImageSector], ax
    mov word ptr cs:[CdImageSector + 2], dx
    mov word ptr cs:[CdImageCount], cx

    push cx
    call CdSetSector
    pop cx

    push cs
    pop ds
    mov si, OFFSET CdBootingMessage
    call SerialWriteString

    /*
     * Into the place a boot sector has always been loaded to. What is counted
     * is halves of a sector, and what is read is whole ones, so the count is
     * brought down to how many of those there are.
     *
     * A disc pretending to be a removable drive says one half of a sector,
     * which is the boot sector of the drive it is pretending to be; reading a
     * whole one is what puts the three after it where they belong as well.
     */
    mov ax, BOOT_SEGMENT
    mov es, ax
    mov bx, BOOT_OFFSET

    add cx, 3
    shr cx, 2

CdBootNext:
    test cx, cx
    jz CdBootLoaded

    call CdReadSector
    jc CdBootNone

    /* On by one sector, and on to the next number */
    add bx, CD_SECTOR_SIZE
    call CdNextSector

    dec cx
    jmp CdBootNext

CdBootLoaded:
    pop es
    pop dx
    pop cx
    pop bx
    clc
    ret

CdBootNone:
    pop es
    pop dx
    pop cx
    pop bx
    stc
    ret

/* WHAT IS SENT AND WHERE IT GOES *********************************************/

/*
 * The command, with only the number in the middle ever changing. It asks for
 * one sector, which is all anything here ever wants at once.
 */
/* Which sector the command is pointing at, held the way round anything counts */
CdSector:
    .byte HEX(00), HEX(00), HEX(00), HEX(00)

CdCommand:
    .byte HEX(28)
    .byte HEX(00)
    .byte HEX(00), HEX(00), HEX(00), HEX(00)
    .byte HEX(00)
    .byte HEX(00), HEX(01)
    .byte HEX(00), HEX(00), HEX(00)

/* The one that asks how much is there, which has nothing in it to fill in */
CdCapacityCommand:
    .byte HEX(25)
    .byte HEX(00)
    .byte HEX(00), HEX(00), HEX(00), HEX(00)
    .byte HEX(00), HEX(00), HEX(00)
    .byte HEX(00), HEX(00), HEX(00)

/* Where its answer lands before being turned round */
CdCapacityAnswer:
    .byte HEX(00), HEX(00), HEX(00), HEX(00)
    .byte HEX(00), HEX(00), HEX(00), HEX(00)

/* And how many there turned out to be */
CdSectorCount:
    .long HEX(00000000)

/* WHAT WAS LOADED ASKS AFTERWARDS ********************************************/

/*
 * The description of how this was booted, which whatever was loaded asks for
 * so that it can read the rest of itself. Everything in it was worked out
 * while the chain was followed.
 */
CdSpecPacket:
    .byte HEX(13)
    .byte HEX(00)
    .byte HEX(E0)
    .byte HEX(00)
CdImageSector:
    .byte HEX(00), HEX(00), HEX(00), HEX(00)
    .word HEX(0000)
    .word HEX(0000)
    .word HEX(0000)
CdImageCount:
    .word HEX(0000)
    .byte HEX(00), HEX(00), HEX(00)
    .byte HEX(00), HEX(00), HEX(00)

/* Hands that description to whoever asked, which is always the loaded image */
Int13CdSpec:
    push ax
    push cx
    push si
    push di
    push ds

    push cs
    pop ds
    mov si, OFFSET CdSpecPacket
    mov cx, HEX(13)
    cld
    rep movsb

    pop ds
    pop di
    pop si
    pop cx
    pop ax

    xor ah, ah
    jmp Int13Succeeded

/*
 * Reading by number rather than by shape, which is the only way to ask a drive
 * of this kind for anything. What to read is described by a block the caller
 * points at rather than by registers, because a number this big does not fit
 * in them.
 */
Int13CdExtendedRead:
    push ax
    push bx
    push cx
    push dx
    push si
    push es

    /* How many, where to, and which, all out of the block */
    mov cx, word ptr ds:[si + 2]
    mov bx, word ptr ds:[si + 4]
    mov ax, word ptr ds:[si + 6]
    mov es, ax

    mov ax, word ptr ds:[si + 8]
    mov dx, word ptr ds:[si + 10]
    call CdSetSector

Int13CdReadNext:
    test cx, cx
    jz Int13CdReadDone

    call CdReadSector
    jc Int13CdReadFailed

    add bx, CD_SECTOR_SIZE
    jnc Int13CdReadSameSegment

    /* Past the end of the segment, so on to the next one */
    push ax
    mov ax, es
    add ax, HEX(1000)
    mov es, ax
    pop ax

Int13CdReadSameSegment:
    call CdNextSector

    dec cx
    jmp Int13CdReadNext

Int13CdReadDone:
    pop es
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    xor ah, ah
    jmp Int13Succeeded

Int13CdReadFailed:
    pop es
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    mov ah, HEX(04)
    jmp Int13Failed

CdFoundMessage:
    .ascii "an optical drive is there"
    .byte 13, 10, 0

CdBootingMessage:
    .ascii "loading from the disc"
    .byte 13, 10, 0

/* PRETENDING TO BE A REMOVABLE DRIVE *****************************************/

/*
 * A disc may carry, instead of something meant to be started off a disc, an
 * image of the removable disk somebody would have started the machine from
 * before there were discs. What is on that image was written when there was
 * nothing else, and asks the firmware for its own sectors by naming a track, a
 * side and a sector on a drive that was the only kind there was.
 *
 * So the drive is made to be there. The numbers it is asked for are turned
 * into a place on the disc and read from there, and nothing that was written
 * for such a drive ever finds out.
 */


/* Whether the drive is being pretended at all */
CdPretending:
    .byte CD_PRETEND_NONE

/* And where on the disc the whole of what it holds begins */
CdPretendAt:
    .byte HEX(00), HEX(00), HEX(00), HEX(00)

/* Whether the disc is answering for the removable drive, with carry clear */
CdIsPretending:
    cmp byte ptr cs:[CdPretending], CD_PRETEND_NONE
    je CdIsPretendingNo
    cmp byte ptr cs:[CdPretending], CD_PRETEND_LARGE
    ja CdIsPretendingNo
    clc
    ret

CdIsPretendingNo:
    stc
    ret

/* How many sectors go round one of its tracks, into AX */
CdPretendRound:
    mov ax, CD_USUAL_SECTORS
    cmp byte ptr cs:[CdPretending], CD_PRETEND_SMALL
    jne CdPretendRoundBig
    mov ax, CD_SMALL_SECTORS
    ret

CdPretendRoundBig:
    cmp byte ptr cs:[CdPretending], CD_PRETEND_LARGE
    jne CdPretendRoundDone
    mov ax, CD_LARGE_SECTORS

CdPretendRoundDone:
    ret

/*
 * Which of its sectors the track in CH, the side in DH and the sector in CL
 * name, counting from the front of it, into DX:AX.
 *
 * The three of them were how a drive was addressed when a drive had a shape
 * anything could see, and this is the sum that undoes it: whole tracks, then
 * whole sides, then however far round the last one.
 */
CdPretendPlace:
    push bx
    push cx
    push si
    push di

    /*
     * Which side, taken out first. What comes back from here is a number too
     * big for one register and so comes back in two, and the second of them
     * is where the side was: every sum below overwrites it.
     */
    movzx di, dh

    /* How many go round a track, which is the one thing that differs by kind */
    call CdPretendRound
    mov bx, ax

    /* Every track before this one, each of them on every side of the drive */
    movzx ax, ch
    mov si, CD_PRETEND_SIDES
    mul si

    /* And the sides of this track that come before this side */
    add ax, di

    mul bx

    /* However far round the track it is, which is counted from one */
    movzx si, cl
    dec si
    add ax, si
    adc dx, 0

    pop di
    pop si
    pop cx
    pop bx
    ret

/*
 * Reads one of its sectors into ES:BX. DX:AX says which, counting from the
 * front of what the disc holds for it.
 *
 * Four of them fit in one of the disc's, so which quarter of which sector is
 * wanted comes out of the same number: the top of it says which to read and
 * the bottom says how far into what came back the wanted part begins.
 */
CdPretendRead:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds

    /* Which quarter of one of the disc's sectors is wanted */
    mov di, ax
    and di, CD_PER_DISC_SECTOR - 1

    /* And which of them, which is the same number with that taken off it */
    shr dx, 1
    rcr ax, 1
    shr dx, 1
    rcr ax, 1

    /* Counted from where what the disc holds for the drive begins */
    add ax, word ptr cs:[CdPretendAt]
    adc dx, word ptr cs:[CdPretendAt + 2]

    call CdSetSector

    /* Read whole, into somewhere of this firmware's own */
    push es
    push bx
    mov ax, CD_SCRATCH_SEGMENT
    mov es, ax
    xor bx, bx
    call CdReadSector
    pop bx
    pop es
    jc CdPretendReadFailed

    /* However far into what came back the wanted quarter begins */
    mov ax, di
    mov cl, 9
    shl ax, cl
    mov si, ax

    mov di, bx
    mov ax, CD_SCRATCH_SEGMENT
    mov ds, ax
    mov cx, CD_PRETEND_SECTOR
    cld
    rep movsb

    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    clc
    ret

CdPretendReadFailed:
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    stc
    ret

/*
 * On to the sector after the one CH, DH and CL name, the way round a drive of
 * that shape counts: round the track, then over to the other side, then on to
 * the next track.
 */
CdPretendStep:
    push ax

    inc cl

    call CdPretendRound
    cmp cl, al
    jbe CdPretendStepDone

    mov cl, 1
    inc dh
    cmp dh, CD_PRETEND_SIDES
    jb CdPretendStepDone

    mov dh, 0
    inc ch

CdPretendStepDone:
    pop ax
    ret
