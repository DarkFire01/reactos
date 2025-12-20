#include "entrypoint.h"

typedef enum _FIXUP
{
    Fix32To64,
    Fix64To32
} FIXUP;

typedef struct _ENTRYPOINT_TRANSLATION
{
    LPCWSTR     LibraryName32;
    LPCWSTR     LibraryName64;
    LPCSTR      SymbolName64;
    LPCSTR      SymbolName32;
    ULONG_PTR   SymbolAddress64;
    ULONG       SymbolAddress32;
    NTSTATUS    (*ContextFixup)(HANDLE hProcess, PCONTEXT, PI386_CONTEXT, FIXUP);
} ENTRYPOINT_TRANSLATION, *PENTRYPOINT_TRANSLATION;

static
NTSTATUS
Wow64FixupNativeEntrypoint(HANDLE hProcess, PCONTEXT pContext);

static 
NTSTATUS
FixupStartupThunks(HANDLE hProcess, 
                   PCONTEXT pContext, 
                   PI386_CONTEXT pContext32, 
                   FIXUP fixup);

static ENTRYPOINT_TRANSLATION EntrypointTranslations[] =
{
    {
        L"kernel32.dll",    L"kernel32.dll", 
        "BaseProcessStart",  "BaseProcessStartThunk", 
        0,                  0, 
        FixupStartupThunks
    },
    {
        L"kernel32.dll",    L"kernel32.dll", 
        "BaseThreadStart",   "BaseThreadStartupThunk",  
        0,                  0, 
        FixupStartupThunks
    }
};

static
NTSTATUS
Wow64MapSelectedTranslationDll(IN LPCWSTR Name,
                               IN OPTIONAL LPCWSTR PrevName,
                               IN LPCWSTR Directory,
                               IN OUT PHANDLE phSection,
                               IN OUT PVOID* Base)
{
    HANDLE hFile;
    NTSTATUS Status;
    OBJECT_ATTRIBUTES ObjectAttributes;
    SIZE_T ViewSize = 0;
    PVOID ViewBase = NULL;
    /* TODO/FIXME: Grow the buffer if needed. */
    WCHAR PathBuffer[MAX_PATH] = { 0 };
    UNICODE_STRING Path;
    IO_STATUS_BLOCK IoStatusBlock = { 0 };
   
    if (PrevName == NULL)
    {
        PrevName = L"";
    }
   
    if (wcscmp(Name, PrevName) != 0)
    {
        wcscpy(PathBuffer, Directory);
        wcscat(PathBuffer, L"\\");
        wcscat(PathBuffer, Name);
        
        Path.Buffer = PathBuffer;
        Path.MaximumLength = sizeof(PathBuffer);
        Path.Length = (USHORT)wcslen(PathBuffer) * sizeof(WCHAR);
        ASSERT(Path.Length <= Path.MaximumLength);
        
        if (*phSection != NULL)
        {
            NtUnmapViewOfSection(NtCurrentProcess(), *Base);
            NtClose(*phSection);
        }
        
        InitializeObjectAttributes(&ObjectAttributes,
                                   &Path,
                                   OBJ_CASE_INSENSITIVE,
                                   NULL,
                                   NULL);
        
        Status = NtOpenFile(&hFile,
                            SYNCHRONIZE | FILE_EXECUTE | FILE_READ_DATA,
                            &ObjectAttributes,
                            &IoStatusBlock,
                            FILE_SHARE_READ | FILE_SHARE_DELETE,
                            FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
        if (!NT_SUCCESS(Status))
        {
            return Status;
        }
                            
        Status = NtCreateSection(phSection,
                                 SECTION_MAP_READ | SECTION_MAP_EXECUTE | 
                                     SECTION_MAP_WRITE | SECTION_QUERY,
                                 NULL,
                                 NULL,
                                 PAGE_EXECUTE,
                                 SEC_IMAGE,
                                 hFile);
        if (!NT_SUCCESS(Status))
        {
            NtClose(hFile);
            return Status;
        }
        
        NtClose(hFile);
        
        Status = NtMapViewOfSection(*phSection,
                                    NtCurrentProcess(),
                                    &ViewBase,
                                    0,
                                    0,
                                    NULL,
                                    &ViewSize,
                                    ViewShare,
                                    0,
                                    PAGE_READWRITE);
        if (!NT_SUCCESS(Status))
        {
            *Base = NULL;
            return Status;
        }
        
        *Base = ViewBase;
    }
    
    return STATUS_SUCCESS;
}

static
NTSTATUS
Wow64FindDllExport(PVOID DllBase,
                   LPCSTR ImportName,
                   PVOID* Result)
{
    PIMAGE_EXPORT_DIRECTORY ExportDir;
    ULONG ExportDirSize;
    
    const UINT16* OrdinalTable;
    const ULONG* NameTable;
    const ULONG* AddressTable;
    UINT16 Ordinal;
    LPCSTR Name;
    
    ULONG i;
    
    /* Get the pointer to the export directory */
    ExportDir = RtlImageDirectoryEntryToData(DllBase,
                                             TRUE,
                                             IMAGE_DIRECTORY_ENTRY_EXPORT,
                                             &ExportDirSize);

    if (!ExportDir)
    {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    NameTable = (PVOID)(ExportDir->AddressOfNames + (ULONG_PTR)DllBase);
    OrdinalTable = (PVOID)(ExportDir->AddressOfNameOrdinals + (ULONG_PTR)DllBase);
    AddressTable = (PVOID)(ExportDir->AddressOfFunctions + (ULONG_PTR)DllBase);

    Ordinal = 0;

    for (i = 0; i < ExportDir->NumberOfNames; i++)
    {
        Name = (LPCSTR)(NameTable[i] + (ULONG_PTR)DllBase);
        
        if (strcmp(Name, ImportName) == 0)
        {
            Ordinal = OrdinalTable[i];
            *Result = (PVOID)((ULONG_PTR)DllBase + AddressTable[Ordinal]);
            
            return STATUS_SUCCESS;
        }
    }
    
    return STATUS_PROCEDURE_NOT_FOUND;
}

NTSTATUS
Wow64InitEntrypointTranslation(VOID)
{
    SIZE_T i = 0;
    PENTRYPOINT_TRANSLATION pTranslation;
    NTSTATUS Status;
    UNICODE_STRING UnexpanedSysRoot = RTL_CONSTANT_STRING(L"%SystemRoot%");
    UNICODE_STRING SystemRootString;
    WCHAR Buffer[MAX_PATH] = { 0 };
    
    PVOID Temp;
    
    PVOID Base32 = NULL, Base64 = NULL;
    
    LPCWSTR PrevName32 = NULL, PrevName64 = NULL;
    WCHAR Directory32[MAX_PATH], Directory64[MAX_PATH];

    /* Get the Windows directory */
    RtlInitEmptyUnicodeString(&SystemRootString, Buffer, sizeof(Buffer));
    Status = RtlExpandEnvironmentStrings_U(NULL,
                                           &UnexpanedSysRoot,
                                           &SystemRootString,
                                           NULL);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }
    
#ifndef TMP_WOW_DIR
    wcscpy(Directory32, L"\\??\\");
    wcscat(Directory32, Buffer);
    wcscat(Directory32, L"\\SysWOW64");
#else
    wcscpy(Directory32, L"\\??\\" TMP_WOW_DIR);
#endif
    
    wcscpy(Directory64, L"\\??\\");
    wcscat(Directory64, Buffer);
    wcscat(Directory64, L"\\System32");
    
    DPRINT1("Translation directories %ls and %ls\n", Directory32, Directory64);
    
    HANDLE hCurrentSection32 = NULL, hCurrentSection64 = NULL;
    
    for (i = 0; i < _countof(EntrypointTranslations); i++)
    {
        pTranslation = &EntrypointTranslations[i];
        
        Status = Wow64MapSelectedTranslationDll(pTranslation->LibraryName32,
                                                PrevName32,
                                                Directory32,
                                                &hCurrentSection32,
                                                &Base32);
        if (!NT_SUCCESS(Status))
        {
            break;
        }
        
        Status = Wow64MapSelectedTranslationDll(pTranslation->LibraryName64,
                                                PrevName64,
                                                Directory64,
                                                &hCurrentSection64,
                                                &Base64);
        if (!NT_SUCCESS(Status))
        {
            break;
        }
        
        
        Status = Wow64FindDllExport(Base32, 
                                    pTranslation->SymbolName32, 
                                    &Temp);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("WOW64 Entrypoint Translation Warning: %s not found in 32-bit %ls\n", 
                    pTranslation->SymbolName32, pTranslation->LibraryName32);
            continue;
        }
        pTranslation->SymbolAddress32 = PtrToUlong(Temp);
        
        Status = Wow64FindDllExport(Base64, 
                                    pTranslation->SymbolName64, 
                                    &Temp);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("WOW64 Entrypoint Translation Warning: %s not found in 64-bit %ls\n", 
                    pTranslation->SymbolName64, pTranslation->LibraryName64);
            continue;
        }
        pTranslation->SymbolAddress64 = (ULONG_PTR)Temp;
        
        DPRINT("Translating %ls!%s=%llX <-> %ls!%s=%lX\n", 
               pTranslation->LibraryName64,
               pTranslation->SymbolName64,
               pTranslation->SymbolAddress64, 
               pTranslation->LibraryName32,
               pTranslation->SymbolName32,
               pTranslation->SymbolAddress32);
        
        PrevName32 = pTranslation->LibraryName32;
        PrevName64 = pTranslation->LibraryName64;
    }
    
    if (hCurrentSection32 != NULL)
    {
        NtUnmapViewOfSection(NtCurrentProcess(), Base32);
        NtClose(hCurrentSection32);
    }
    if (hCurrentSection64 != NULL)
    {
        NtUnmapViewOfSection(NtCurrentProcess(), Base64);
        NtClose(hCurrentSection64);
    }
    
    return Status;
}

NTSTATUS
Wow64TranslateEntrypoint32To64(IN  HANDLE hProcess,
                               OUT PCONTEXT pContext, 
                               IN  PI386_CONTEXT pContext32)
{
    SIZE_T i;
    PENTRYPOINT_TRANSLATION pTranslation;
    NTSTATUS Status;
    
    for (i = 0; i < _countof(EntrypointTranslations); i++)
    {
        pTranslation = &EntrypointTranslations[i];
        
        if (pTranslation->SymbolAddress32 == pContext32->Eip)
        {
            if (pTranslation->ContextFixup != NULL)
            {
                Status = pTranslation->ContextFixup(hProcess, 
                                                    pContext, 
                                                    pContext32, 
                                                    Fix32To64);
                if (!NT_SUCCESS(Status))
                {
                    return Status;
                }
            }
            
            pContext->Rip = pTranslation->SymbolAddress64;
            return STATUS_SUCCESS;
        }
    }

    pContext->Rip = pContext32->Eip;
    return STATUS_SUCCESS;
}

NTSTATUS
Wow64TranslateEntrypoint64To32(IN  HANDLE hProcess,
                               OUT PI386_CONTEXT pContext32, 
                               IN  PCONTEXT pContext)
{
    SIZE_T i;
    NTSTATUS Status;
    PENTRYPOINT_TRANSLATION pTranslation;
    
    for (i = 0; i < _countof(EntrypointTranslations); i++)
    {
        pTranslation = &EntrypointTranslations[i];
        
        if (pTranslation->SymbolAddress64 == pContext->Rip)
        {
            if (pTranslation->ContextFixup != NULL)
            {
                Status = pTranslation->ContextFixup(hProcess, 
                                                    pContext, 
                                                    pContext32, 
                                                    Fix64To32);
                if (!NT_SUCCESS(Status))
                {
                    return Status;
                }
            }
            
            pContext32->Eip = pTranslation->SymbolAddress32;
            return STATUS_SUCCESS;
        }
    }
    
    pContext32->Eip = pContext->Rip;
    return STATUS_SUCCESS;
}

static 
NTSTATUS 
FixupStartupThunks(HANDLE hProcess, 
                   PCONTEXT pContext, 
                   PI386_CONTEXT pContext32, 
                   FIXUP fixup)
{
    NTSTATUS Status;
    
    switch (fixup)
    {
        case Fix32To64:
            pContext->Rcx = pContext32->Eax;
            pContext->Rdx = pContext32->Ebx;
            pContext->SegCs = 0x33;
            pContext->Rsp &= (~0xFULL);
            pContext->Rsp -= 8;
            
            /* Manually fixup the entrypoint; see wow64_NtQuerySection */
            if (pContext->Rcx == 0x81231234)
            {
                Status = Wow64FixupNativeEntrypoint(hProcess, pContext);
                if (!NT_SUCCESS(Status))
                {
                    return Status;
                }
            }
            
            break;
        case Fix64To32:
            pContext32->Eax = pContext->Rcx;
            pContext32->Ebx = pContext->Rdx;
            pContext32->SegCs = 0x23;
            break;
    }
    
    return STATUS_SUCCESS;
}

static
NTSTATUS
Wow64FixupNativeEntrypoint(HANDLE hProcess, PCONTEXT pContext)
{
    PROCESS_BASIC_INFORMATION Info;
    NTSTATUS Status;
    PPEB RemotePeb;
    ULONG_PTR BaseAddress;
    ULONG eLfanew;
    ULONG_PTR NtHeaders;
    WORD OptionalHeaderMagic;
    DWORD EntrypointRva;
    
    __debugbreak();
    
    /* Get the remote PEB */
    Status = NtQueryInformationProcess(hProcess, 
                                       ProcessBasicInformation, 
                                       &Info,
                                       sizeof(Info), 
                                       NULL);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }
    
    RemotePeb = Info.PebBaseAddress;
    
    /* Get the remote base address */
    Status = NtReadVirtualMemory(hProcess,
                                 &RemotePeb->ImageBaseAddress,
                                 &BaseAddress,
                                 sizeof(BaseAddress),
                                 NULL);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }
    
    /* Find NT headers */
    Status = NtReadVirtualMemory(hProcess,
                                 &((PIMAGE_DOS_HEADER)BaseAddress)->e_lfanew,
                                 &eLfanew,
                                 sizeof(eLfanew),
                                 NULL);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }
    
    NtHeaders = BaseAddress + eLfanew;
    
    /* Validate the image */
    Status = NtReadVirtualMemory(hProcess,
                                 &((PIMAGE_NT_HEADERS64)NtHeaders)->OptionalHeader.Magic,
                                 &OptionalHeaderMagic,
                                 sizeof(OptionalHeaderMagic),
                                 NULL);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }
    ASSERT(OptionalHeaderMagic == IMAGE_NT_OPTIONAL_HDR64_MAGIC);
    
    Status = NtReadVirtualMemory(hProcess,
                                 &((PIMAGE_NT_HEADERS64)NtHeaders)->OptionalHeader.
                                    AddressOfEntryPoint,
                                 &EntrypointRva,
                                 sizeof(EntrypointRva),
                                 NULL);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }
    
    pContext->Rcx = BaseAddress + EntrypointRva;
    
    return STATUS_SUCCESS;
}