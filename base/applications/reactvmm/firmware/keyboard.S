/*
 * The keys, as the firmware takes them.
 *
 * What arrives on the wire is not a character, it is which key moved and which
 * way. Turning that into a character is the firmware's work and always has
 * been, which is why the table below is here and not in the controller.
 */

#define KBD_DATA            HEX(0060)
#define KBD_STATUS          HEX(0064)

/* What the status register says when there is a byte to take */
#define KBD_OUTPUT_FULL     HEX(01)

/* The codes for the keys that change what another key means */
#define KBD_LEFT_SHIFT      HEX(2A)
#define KBD_RIGHT_SHIFT     HEX(36)
#define KBD_CONTROL         HEX(1D)
#define KBD_ALT             HEX(38)

/* The bit in a code that says the key came back up */
#define KBD_RELEASED        HEX(80)

/* How far the table below reaches, which is as far as a key makes a character */
#define KeyTableLength      HEX(3A)

/* TAKING ONE *****************************************************************/

/*
 * A key moved. What it turns into is kept for whoever asks next, and anything
 * that is not a character is dropped: a firmware that handed back a zero for
 * every shift press would have a caller take it for a key.
 */
Int09Handler:
    push ax
    push bx
    push ds

    push cs
    pop ds

    in al, KBD_DATA

    /* Either half of a shift, which is a state rather than a character */
    mov bl, al
    and bl, NOT KBD_RELEASED

    cmp bl, KBD_LEFT_SHIFT
    je Int09Shift
    cmp bl, KBD_RIGHT_SHIFT
    je Int09Shift
    cmp bl, KBD_CONTROL
    je Int09Control
    cmp bl, KBD_ALT
    je Int09Done

    /* Nothing is made of a key coming back up */
    test al, KBD_RELEASED
    jnz Int09Done

    /* Past the end of what the table describes */
    cmp al, KeyTableLength
    jae Int09Done

    movzx bx, al
    cmp byte ptr ds:[KeyShift], 0
    jne Int09Shifted

    mov al, byte ptr ds:[bx + OFFSET KeyPlain]
    jmp Int09Made

Int09Shifted:
    mov al, byte ptr ds:[bx + OFFSET KeyShifted]

Int09Made:
    test al, al
    jz Int09Done

    /*
     * Held for whoever asks, which is one key deep and always has been enough.
     * The code is kept beside the character, because a caller that wants a key
     * with no character of its own has nothing else to go on.
     */
    mov byte ptr ds:[KeyHeld], al
    mov byte ptr ds:[KeyCode], bl
    mov byte ptr ds:[KeyWaiting], 1

Int09Done:
    mov al, HEX(20)
    out HEX(20), al

    pop ds
    pop bx
    pop ax
    iret

Int09Shift:
    /* Down sets it, up clears it, which is what the top bit already says */
    test al, KBD_RELEASED
    jnz Int09ShiftUp
    mov byte ptr ds:[KeyShift], 1
    jmp Int09Done

Int09ShiftUp:
    mov byte ptr ds:[KeyShift], 0
    jmp Int09Done

Int09Control:
    test al, KBD_RELEASED
    jnz Int09ControlUp
    mov byte ptr ds:[KeyControl], 1
    jmp Int09Done

Int09ControlUp:
    mov byte ptr ds:[KeyControl], 0
    jmp Int09Done

/* WHAT EACH KEY MEANS ********************************************************/

/*
 * The first set, as the keys are laid out on a board anything of this kind was
 * sold with. A zero is a key that makes no character: the ones that move the
 * cursor and the ones along the top are read by their code rather than by what
 * they stand for, and nothing here needs them yet.
 */
KeyPlain:
    /* 00 */ .byte 0, HEX(1B), HEX(31), HEX(32), HEX(33), HEX(34), HEX(35), HEX(36)
    /* 08 */ .byte HEX(37), HEX(38), HEX(39), HEX(30), HEX(2D), HEX(3D), HEX(08), HEX(09)
    /* 10 */ .byte HEX(71), HEX(77), HEX(65), HEX(72), HEX(74), HEX(79), HEX(75), HEX(69)
    /* 18 */ .byte HEX(6F), HEX(70), HEX(5B), HEX(5D), HEX(0D), 0, HEX(61), HEX(73)
    /* 20 */ .byte HEX(64), HEX(66), HEX(67), HEX(68), HEX(6A), HEX(6B), HEX(6C), HEX(3B)
    /* 28 */ .byte HEX(27), HEX(60), 0, HEX(5C), HEX(7A), HEX(78), HEX(63), HEX(76)
    /* 30 */ .byte HEX(62), HEX(6E), HEX(6D), HEX(2C), HEX(2E), HEX(2F), 0, HEX(2A)
    /* 38 */ .byte 0, HEX(20)

KeyShifted:
    /* 00 */ .byte 0, HEX(1B), HEX(21), HEX(40), HEX(23), HEX(24), HEX(25), HEX(5E)
    /* 08 */ .byte HEX(26), HEX(2A), HEX(28), HEX(29), HEX(5F), HEX(2B), HEX(08), HEX(09)
    /* 10 */ .byte HEX(51), HEX(57), HEX(45), HEX(52), HEX(54), HEX(59), HEX(55), HEX(49)
    /* 18 */ .byte HEX(4F), HEX(50), HEX(7B), HEX(7D), HEX(0D), 0, HEX(41), HEX(53)
    /* 20 */ .byte HEX(44), HEX(46), HEX(47), HEX(48), HEX(4A), HEX(4B), HEX(4C), HEX(3A)
    /* 28 */ .byte HEX(22), HEX(7E), 0, HEX(7C), HEX(5A), HEX(58), HEX(43), HEX(56)
    /* 30 */ .byte HEX(42), HEX(4E), HEX(4D), HEX(3C), HEX(3E), HEX(3F), 0, HEX(2A)
    /* 38 */ .byte 0, HEX(20)

KeyCode:
    .byte 0
KeyShift:
    .byte 0
KeyControl:
    .byte 0
