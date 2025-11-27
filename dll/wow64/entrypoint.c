#include "entrypoint.h"

typedef struct _ENTRYPOINT_TRANSLATION
{
    LPCWSTR     LibraryName32;
    LPCWSTR     LibraryName64;
    LPCSTR      SymbolName64;
    LPCSTR      SymbolName32;
    ULONG_PTR   SymbolAddress64;
    ULONG       SymbolAddress32;
} ENTRYPOINT_TRANSLATION, *PENTRYPOINT_TRANSLATION;

static ENTRYPOINT_TRANSLATION EntrypointTranslations[] =
{
    {L"kernel32.dll", L"kernel32.dll", "BaseProcessStart", "BaseProcessStartThunk", 0, 0},
    {L"kernel32.dll", L"kernel32.dll", "BaseThreadStart",  "BaseThreadStartupThunk",  0, 0}
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
    
    PVOID Temp;
    
    PVOID Base32 = NULL, Base64 = NULL;
    
    LPCWSTR PrevName32 = NULL, PrevName64 = NULL;
    WCHAR Directory32[MAX_PATH], Directory64[MAX_PATH];

    /* FIXME */
    wcscpy(Directory32, L"\\??\\" TMP_WOW_DIR);
    wcscpy(Directory64, L"\\??\\C:\\reactos\\system32");
    
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
        
        DPRINT("Translating %s!%s=%llX <-> %s!%s=%lX\n", 
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
        NtClose(hCurrentSection32);
    }
    if (hCurrentSection64 != NULL)
    {
        NtClose(hCurrentSection64);
    }
    
    return Status;
}

ULONG_PTR
Wow64TranslateEntrypoint32To64(ULONG Entrypoint)
{
    SIZE_T i;
    PENTRYPOINT_TRANSLATION pTranslation;
    
    for (i = 1; i < _countof(EntrypointTranslations); i++)
    {
        pTranslation = &EntrypointTranslations[i];
        
        if (pTranslation->SymbolAddress32 == Entrypoint)
        {
            return pTranslation->SymbolAddress64;
        }
    }

    return Entrypoint;
}

ULONG
Wow64TranslateEntrypoint64To32(ULONG_PTR Entrypoint)
{
    SIZE_T i;
    PENTRYPOINT_TRANSLATION pTranslation;
    
    for (i = 1; i < _countof(EntrypointTranslations); i++)
    {
        pTranslation = &EntrypointTranslations[i];
        
        if (pTranslation->SymbolAddress64 == Entrypoint)
        {
            return pTranslation->SymbolAddress32;
        }
    }
    
    return Entrypoint;
}