#include "ros_wow64_private.h"
 
BOOLEAN 
RedirectPath(const WOW64_PATH_REDIRECTION* Redirection, 
             POBJECT_ATTRIBUTES ObjectAttributes)
{
    PUNICODE_STRING ObjectName = ObjectAttributes->ObjectName;
    const UNICODE_STRING* FromUnexpanded = &Redirection->From;
    const UNICODE_STRING* ToUnexpanded = &Redirection->To;
    
    NTSTATUS Status;
    
    UNICODE_STRING From, To;
    WCHAR FromBuffer[MAX_PATH] = { 0 };
    WCHAR ToBuffer[MAX_PATH] = { 0 };
    
    PUNICODE_STRING Buffer = NULL;
    USHORT NewLength;

    RtlInitEmptyUnicodeString(&To, ToBuffer, sizeof(ToBuffer));
    Status = RtlExpandEnvironmentStrings_U(NULL,
                                           (PUNICODE_STRING)ToUnexpanded,
                                           &To,
                                           NULL);
    ASSERT(NT_SUCCESS(Status));
    
    RtlInitEmptyUnicodeString(&From, FromBuffer, sizeof(FromBuffer));
    Status = RtlExpandEnvironmentStrings_U(NULL,
                                           (PUNICODE_STRING)FromUnexpanded,
                                           &From,
                                           NULL);
    ASSERT(NT_SUCCESS(Status));
    
    NewLength = ObjectName->Length - From.Length + To.Length;

    Buffer = Wow64AllocateTemp(sizeof(*Buffer) + NewLength);
    ASSERT(Buffer != NULL);

    Buffer->Buffer = (PWCHAR)(((ULONG_PTR)Buffer) + sizeof(*Buffer));
    
    if (_wcsnicmp(ObjectName->Buffer, From.Buffer, From.Length / sizeof(WCHAR)) == 0)
    {
        Buffer->Length = NewLength;
        
        RtlCopyMemory(Buffer->Buffer, To.Buffer, To.Length);
        
        RtlCopyMemory(Buffer->Buffer + To.Length / sizeof(WCHAR), 
                      ObjectName->Buffer + From.Length / sizeof(WCHAR),
                      ObjectName->Length - From.Length);

        ObjectAttributes->ObjectName = Buffer;  
        return TRUE;
    }
    return FALSE;
}
 
BOOLEAN 
GetFileRedirect(OBJECT_ATTRIBUTES* attr)
{
    size_t i;

    if (PtrToUlong(NtCurrentTeb()->TlsSlots[WOW64_TLS_FILESYSREDIR]) == 0)
    {
        return FALSE;
    }
    
    static const WOW64_PATH_REDIRECTION Redirections[] = 
    {
    /* TODO: system directory shouldn't be hardcoded here */
#define REDIRECTION(From, To) { RTL_CONSTANT_STRING(From), RTL_CONSTANT_STRING(To) }
#ifdef TMP_WOW_DIR
        REDIRECTION(L"\\??\\%SystemRoot%\\system32", L"\\??\\" TMP_WOW_DIR),
#else
        REDIRECTION(L"\\??\\%SystemRoot%\\system32", L"\\??\\%SystemRoot%\\SysWOW64"),
#endif
        REDIRECTION(L"\\KnownDlls", L"\\KnownDlls32")
#undef  REDIRECTION
    };
    
    PUNICODE_STRING ObjectName = attr->ObjectName;
    
    if (!attr || !ObjectName || !ObjectName->Buffer)
    {
        return FALSE;
    }
    
    for (i = 0; i < sizeof(Redirections) / sizeof(*Redirections); i++)
    {
        if (RedirectPath(&Redirections[i], attr))
        {
            return TRUE;
        }
    }

    return FALSE;
}