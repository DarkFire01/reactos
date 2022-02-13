#include "../pcdebug.h"

VOID
RefiDebugInit(ULONG RefiPhase)
{
    if (RefiPhase == 0)
    {
        /* Use default COM settings to get started */
        Rs232PortInitialize(0, 115200);
    }
    else{
        /* TODO: UNIMPLEMENTED */
    }
}

VOID
RefiDebugPrint(CHAR16* str)
{
    ULONG stringLength;
    for(stringLength = 0; stringLength < 64; stringLength++)
    {
        Rs232PortPutByte(str[stringLength]);
    }
}