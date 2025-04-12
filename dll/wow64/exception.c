/*
 * Wine WOW64 ReactOS port 
 *
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         wow64.dll
 * FILE:            dll/wow64/exception.c
 * PROGRAMMER:      Marcin Jabłoński
 */

#include "ros_wow64_private.h"

NTSTATUS
WINAPI
wow64_NtRaiseException(UINT* pArgs)
{
    PEXCEPTION_RECORD32 pRecord32 = get_ptr(&pArgs);
    PI386_CONTEXT pContext32 = get_ptr(&pArgs);
    BOOLEAN FirstChance = get_ulong(&pArgs);
    
    EXCEPTION_RECORD Record;
    CONTEXT Context;

    Record.ExceptionCode = pRecord32->ExceptionCode;
    Record.ExceptionFlags = pRecord32->ExceptionFlags;
    Record.ExceptionRecord = UlongToPtr(pRecord32->ExceptionRecord);
    Record.ExceptionAddress = UlongToPtr(pRecord32->ExceptionAddress);
    Record.NumberParameters = pRecord32->NumberParameters;
    
    CopyContext32To64(&Context, pContext32);
    
    for (int i = 0; i < EXCEPTION_MAXIMUM_PARAMETERS; i++)
    {
        Record.ExceptionInformation[i] = pRecord32->ExceptionInformation[i];
    }

    if (FirstChance == FALSE)
    {
        DPRINT1("Unhandled 32-bit exception in a WOW64 process.\n");
        __debugbreak();
    }

    return NtRaiseException(&Record, &Context, FirstChance);
}

LONG
Wow64UnhandledExceptionHandler(IN PEXCEPTION_POINTERS ExceptionInfo)
{    
    ULONG i;
    EXCEPTION_RECORD32 Record32;
    I386_CONTEXT Context32;
    PCONTEXT pContext;
    PEXCEPTION_RECORD pRecord;
    
    ULONG DispatcherArgs[2];
    
    pRecord = ExceptionInfo->ExceptionRecord;
    pContext = ExceptionInfo->ContextRecord;
    
    Record32.ExceptionCode = pRecord->ExceptionCode;
    Record32.ExceptionFlags = pRecord->ExceptionFlags;
    Record32.ExceptionRecord = PtrToUlong(pRecord->ExceptionRecord);
    Record32.ExceptionAddress = PtrToUlong(pRecord->ExceptionAddress);
    Record32.NumberParameters = pRecord->NumberParameters;
        
    for (i = 0; i < Record32.NumberParameters; i++)
    {
        Record32.ExceptionInformation[i] = pRecord->ExceptionInformation[i];
    }
    
    CopyContext64To32(&Context32, pContext);
    
    DispatcherArgs[0] = PtrToUlong(&Record32);
    DispatcherArgs[1] = PtrToUlong(&Context32);
    
    __debugbreak();
    Jump32(PtrToUlong(NtDll32KiUserExceptionDispatcher), 2, DispatcherArgs);
    
    return EXCEPTION_EXECUTE_HANDLER;
}