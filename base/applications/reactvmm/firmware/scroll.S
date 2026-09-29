/*
 * Moving part of the screen, which is how anything that draws a box in the
 * middle of one clears it and scrolls what is inside it.
 *
 * The rectangle is the whole point. A program that asked for its own window to
 * be cleared and had the entire screen scrolled instead loses everything around
 * it and leaves its own colour in rows it never touched, which is what bands of
 * somebody else's background across a screen are.
 */

/* Where the corners and the count are kept while the rows are walked */
ScrollLines:
    .byte 0
ScrollTop:
    .byte 0
ScrollLeft:
    .byte 0
ScrollBottom:
    .byte 0
ScrollRight:
    .byte 0
ScrollRow:
    .byte 0
ScrollFill:
    .word HEX(0720)

/*
 * Takes the corners out of CX and DX and holds them somewhere the row walk can
 * reach, with anything past the edge of the screen brought back to it. A
 * caller that asks for a window bigger than the screen means the screen.
 */
ScrollTakeWindow:
    push ax

    mov byte ptr cs:[ScrollTop], ch
    mov byte ptr cs:[ScrollLeft], cl
    mov byte ptr cs:[ScrollBottom], dh
    mov byte ptr cs:[ScrollRight], dl

    mov al, byte ptr cs:[ScrollBottom]
    cmp al, VIDEO_ROWS
    jb ScrollBottomFits
    mov byte ptr cs:[ScrollBottom], VIDEO_ROWS - 1

ScrollBottomFits:
    mov al, byte ptr cs:[ScrollRight]
    cmp al, VIDEO_COLUMNS
    jb ScrollRightFits
    mov byte ptr cs:[ScrollRight], VIDEO_COLUMNS - 1

ScrollRightFits:
    /* The colour the lines it gains are left in */
    mov byte ptr cs:[ScrollFill], HEX(20)
    mov byte ptr cs:[ScrollFill + 1], bh

    pop ax
    ret

/* Answers in AX with the offset of the cell at row AL, column AH */
ScrollCellOffset:
    push bx
    push cx
    push dx

    push ax
    movzx ax, al
    mov cx, VIDEO_COLUMNS * 2
    mul cx
    mov bx, ax
    pop ax

    movzx ax, ah
    shl ax, 1
    add ax, bx

    pop dx
    pop cx
    pop bx
    ret

/* How many cells wide the window is, in CX */
ScrollWindowWidth:
    push ax

    movzx cx, byte ptr cs:[ScrollRight]
    movzx ax, byte ptr cs:[ScrollLeft]
    sub cx, ax
    inc cx

    pop ax
    ret

/* Fills the run of the row in AL with whatever the caller asked for */
ScrollBlankRow:
    push ax
    push cx
    push di
    push es

    mov ah, byte ptr cs:[ScrollLeft]
    call ScrollCellOffset
    mov di, ax

    mov ax, VIDEO_SEGMENT
    mov es, ax

    call ScrollWindowWidth
    mov ax, word ptr cs:[ScrollFill]
    cld
    rep stosw

    pop es
    pop di
    pop cx
    pop ax
    ret

/* Empties every row of the window, which is what no count at all means */
ScrollBlankWindow:
    push ax

    mov al, byte ptr cs:[ScrollTop]

ScrollBlankNext:
    cmp al, byte ptr cs:[ScrollBottom]
    ja ScrollBlankDone
    call ScrollBlankRow
    inc al
    jmp ScrollBlankNext

ScrollBlankDone:
    pop ax
    ret

/*
 * Whether the count asks for more than the window holds. Carry set means it
 * does, and a caller that asked for that meant to empty the whole of it.
 */
ScrollCountFits:
    push ax
    push bx

    mov al, byte ptr cs:[ScrollLines]
    test al, al
    jz ScrollCountTooMany

    mov bl, byte ptr cs:[ScrollBottom]
    sub bl, byte ptr cs:[ScrollTop]
    inc bl

    cmp al, bl
    jae ScrollCountTooMany

    pop bx
    pop ax
    clc
    ret

ScrollCountTooMany:
    pop bx
    pop ax
    stc
    ret

/* THE TWO CALLS **************************************************************/

/*
 * AL lines, or none to mean the whole window. BH is the colour, and CX and DX
 * are the upper left and lower right corners.
 */
Int10ScrollUp:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    mov byte ptr cs:[ScrollLines], al
    call ScrollTakeWindow
    call ScrollCountFits
    jc Int10ScrollUpClear

    /*
     * Every row takes the one that many below it, working downward so that a
     * row is read before whatever is about to be written over it.
     */
    mov al, byte ptr cs:[ScrollTop]

Int10ScrollUpNext:
    mov ah, al
    add ah, byte ptr cs:[ScrollLines]
    cmp ah, byte ptr cs:[ScrollBottom]
    ja Int10ScrollUpTail

    mov byte ptr cs:[ScrollRow], al
    push ax
    mov al, ah
    call ScrollRowInto
    pop ax

    inc al
    jmp Int10ScrollUpNext

Int10ScrollUpTail:
    /* Whatever is left at the bottom is what the window gained */
Int10ScrollUpBlank:
    cmp al, byte ptr cs:[ScrollBottom]
    ja Int10ScrollUpDone
    call ScrollBlankRow
    inc al
    jmp Int10ScrollUpBlank

Int10ScrollUpClear:
    call ScrollBlankWindow

Int10ScrollUpDone:
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    iret

/* The same the other way, for a list that is being walked back up */
Int10ScrollDown:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    mov byte ptr cs:[ScrollLines], al
    call ScrollTakeWindow
    call ScrollCountFits
    jc Int10ScrollDownClear

    /* Upward this time, for the same reason the other one goes down */
    mov al, byte ptr cs:[ScrollBottom]

Int10ScrollDownNext:
    mov ah, al
    sub ah, byte ptr cs:[ScrollLines]
    cmp ah, byte ptr cs:[ScrollTop]
    jb Int10ScrollDownTail

    mov byte ptr cs:[ScrollRow], al
    push ax
    mov al, ah
    call ScrollRowInto
    pop ax

    dec al
    jmp Int10ScrollDownNext

Int10ScrollDownTail:
Int10ScrollDownBlank:
    cmp al, byte ptr cs:[ScrollTop]
    jb Int10ScrollDownDone
    call ScrollBlankRow
    dec al
    cmp al, HEX(FF)
    je Int10ScrollDownDone
    jmp Int10ScrollDownBlank

Int10ScrollDownClear:
    call ScrollBlankWindow

Int10ScrollDownDone:
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    iret

/*
 * Copies the window's run of the row in AL into the row named by ScrollRow.
 * Kept apart from the two above because both need it and neither has a
 * register left to say where the copy is going.
 */
ScrollRowInto:
    push ax
    push cx
    push si
    push di
    push ds
    push es

    mov ah, byte ptr cs:[ScrollLeft]
    call ScrollCellOffset
    mov si, ax

    mov al, byte ptr cs:[ScrollRow]
    mov ah, byte ptr cs:[ScrollLeft]
    call ScrollCellOffset
    mov di, ax

    mov ax, VIDEO_SEGMENT
    mov ds, ax
    mov es, ax

    call ScrollWindowWidth
    cld
    rep movsw

    pop es
    pop ds
    pop di
    pop si
    pop cx
    pop ax
    ret
