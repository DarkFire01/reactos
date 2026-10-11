#include "ros_wow64_private.h"

NTSTATUS
WINAPI
wow64_NtContinue(UINT *pArgs)
{
    PWOW64_CONTEXT pContext32 = get_ptr(&pArgs);
    BOOLEAN bRasieAlert = get_ulong(&pArgs);

    NTSTATUS Status = pContext32->Eax;
    struct user_apc_frame *Frame = NtCurrentTeb()->TlsSlots[WOW64_TLS_APCLIST];

    pBTCpuSetContext(NtCurrentThread(), NtCurrentProcess(), NULL, pContext32);

    while (Frame && Frame->wow_context != pContext32)
    {
        Frame = Frame->prev_frame;
    } 

    NtCurrentTeb()->TlsSlots[WOW64_TLS_APCLIST] = Frame ? Frame->prev_frame : NULL;
    
    if (Frame) 
    {   
        NtContinue(Frame->context, bRasieAlert);
    }

    if (bRasieAlert) NtTestAlert();
    return Status;
}

VOID 
WINAPI 
Wow64ApcRoutine(ULONG_PTR SystemArgument1, 
                ULONG_PTR SystemArgument2, 
                ULONG_PTR SystemArgument3, 
                PCONTEXT  Context)
{
#pragma pack(push, 1)
    struct Enter32
    {
        ULONG_PTR       Rip32;
        ULONG_PTR       SegCs32;
        ULONG           FarReturn32;
        ULONG           Func;
        UINT            SystemArgument1;
        UINT            SystemArgument2;
        UINT            SystemArgument3;
        UINT            Alertable;
        WOW64_CONTEXT   Context32;
        struct user_apc_frame Frame;
    };
    
    struct Leave32
    {
        ULONG Rip64;
        ULONG SegCs64;
    };        

    ULONG_PTR Enter32(PVOID enter);
    VOID FarReturn32();
    
    struct Leave32* leave;
    struct Enter32* enter;
    
    BYTE StackBuffer[sizeof(struct Enter32) + sizeof(struct Leave32)];
    
    leave = (struct Leave32*)(StackBuffer + sizeof(StackBuffer) - sizeof(*leave));
    enter = (struct Enter32*)(StackBuffer + sizeof(StackBuffer) - sizeof(*leave) - sizeof(*enter));
#pragma pack(pop)
    
    enter->FarReturn32 = PtrToUlong(FarReturn32);
    
    enter->Frame.prev_frame = NtCurrentTeb()->TlsSlots[WOW64_TLS_APCLIST];
    enter->Frame.context    = Context;
    NtCurrentTeb()->TlsSlots[WOW64_TLS_APCLIST] = &enter->Frame;

    Wow64CopyContext64To32(&enter->Context32, Context);

    enter->Func = SystemArgument1 >> 32;
    enter->SystemArgument1 = SystemArgument1;
    enter->SystemArgument2 = SystemArgument2;
    enter->SystemArgument3 = SystemArgument3;
    enter->Alertable = TRUE;
    enter->Frame.wow_context = &enter->Context32;

    enter->Rip32 = (ULONG_PTR) NtDll32KiUserApcDispatcher;
    enter->SegCs32 = 0x23;
    
    leave->Rip64 = PtrToUlong(_ReturnAddress());
    leave->SegCs64 = 0x33;
    
    Enter32(enter);
    
    /* We should never get here. */
    ASSERT(FALSE);
}