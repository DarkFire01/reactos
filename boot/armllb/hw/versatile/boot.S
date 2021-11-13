
    #include <kxarm.h>

    TEXTAREA
	LEAF_ENTRY ARMLLBEntryPoint
    /* EXTERN __LlbStartup[WEAK] */

    /* ldr r4, __LlbStartup */
	ldr lr, L_LlbStartup
 	ldr sp, L_BootStackEnd
	bx lr

    LEAF_END ARMLLBEntryPoint

L_BootStackEnd dcq 0x00010000
L_LlbStartup

    END
/* EOF */
