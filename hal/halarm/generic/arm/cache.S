/*
 * PROJECT:         ReactOS HAL
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            hal/halarm/generic/cache.S
 * PURPOSE:         Implements cache clean, invalidate routines for ARM machines
 * PROGRAMMERS:     Copyright (C) 2005 ARM Ltd.
 */
#ifdef _M_ARM64
#include <ksarm64.h>

    TEXTAREA

    NESTED_ENTRY v7_flush_dcache_all
    PROLOG_END
    mrs     x0, clidr_el1           ; read CLIDR_EL1
    and     x3, x0, #0x7000000     ; extract LoC (bits [26:24])
    lsr     x3, x3, #23            ; shift left to get LoC * 2
    cbz     x3, a64_finished       ; if LoC is 0, no work to do
    mov     x10, #0                ; cache level * 2 (start at level 0)
a64_loop1
    add     x2, x10, x10, lsr #1  ; work out 3x current cache level
    lsr     x1, x0, x2             ; extract cache type bits from CLIDR
    and     x1, x1, #7            ; mask for current cache only
    cmp     x1, #2                 ; see what cache we have
    b.lt    a64_skip               ; skip if no cache or only i-cache
    msr     csselr_el1, x10        ; select current cache level in CSSELR
    isb                            ; sync the new CSSELR
    mrs     x1, ccsidr_el1         ; read CCSIDR_EL1
    and     x2, x1, #7            ; extract line length field
    add     x2, x2, #4             ; add 4 (line length offset)
    ubfx    x4, x1, #3, #10       ; way count (bits [12:3])
    clz     w5, w4                 ; bit position of way size increment
    ubfx    x7, x1, #13, #15      ; set count (bits [27:13])
a64_loop2
    mov     x9, x4                 ; working copy of max way
a64_loop3
    lsl     x6, x9, x5
    orr     x11, x10, x6
    lsl     x6, x7, x2
    orr     x11, x11, x6
    dc      cisw, x11              ; clean & invalidate by set/way
    subs    x9, x9, #1
    b.ge    a64_loop3
    subs    x7, x7, #1
    b.ge    a64_loop2
a64_skip
    add     x10, x10, #2           ; increment cache number
    cmp     x3, x10
    b.gt    a64_loop1
a64_finished
    msr     csselr_el1, xzr        ; switch back to cache level 0
    dsb     sy
    isb
    ret
    NESTED_END v7_flush_dcache_all

    END

#else
#include <ksarm.h>

    TEXTAREA

    NESTED_ENTRY v7_flush_dcache_all
    PROLOG_END v7_flush_dcache_all

	mrc	p15, 1, r0, c0, c0, 1		// read clidr
	ands	r3, r0, #0x7000000		// extract loc from clidr
	mov	r3, r3, lsr #23             // left align loc bit field
	beq	finished                    // if loc is 0, then no need to clean
	mov	r10, #0                     // start clean at cache level 0
loop1
	add	r2, r10, r10, lsr #1		// work out 3x current cache level
	mov	r1, r0, lsr r2              // extract cache type bits from clidr
	and	r1, r1, #7                  // mask of the bits for current cache only
	cmp	r1, #2                      // see what cache we have at this level
	blt	skip                        // skip if no cache, or just i-cache
	mcr	p15, 2, r10, c0, c0, 0		// select current cache level in cssr
	isb                             // isb to sych the new cssr&csidr
	mrc	p15, 1, r1, c0, c0, 0		// read the new csidr
	and	r2, r1, #7                  // extract the length of the cache lines
	add	r2, r2, #4                  // add 4 (line length offset)
	ldr	r4, =0x3ff
	ands	r4, r4, r1, lsr #3		// find maximum number on the way size
	clz	r5, r4                      // find bit position of way size increment
	ldr	r7, =0x7fff
	ands	r7, r7, r1, lsr #13		// extract max number of the index size
loop2
	mov	r9, r4                      // create working copy of max way size
loop3
	orr	r11, r10, r9		// factor way and cache number into r11
	orr	r11, r11, r7		// factor index number into r11
	mcr	p15, 0, r11, c7, c14, 2		// clean & invalidate by set/way
	subs	r9, r9, #1              // decrement the way
	bge	loop3
	subs	r7, r7, #1              // decrement the index
	bge	loop2
skip
	add	r10, r10, #2                // increment cache number
	cmp	r3, r10
	bgt	loop1
finished
	mov	r10, #0                     // swith back to cache level 0
	mcr	p15, 2, r10, c0, c0, 0		// select current cache level in cssr
	isb
	mov	pc, lr

    NESTED_END v7_flush_dcache_all
 	END

#endif
