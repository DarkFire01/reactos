/*
 * PROJECT:     ReactOS host tools
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Rewrites COFF objects so the ARM64 and ARM64EC halves of an ARM64X image share their writable data
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* Usage: arm64xobj -native|-ec <source file> <object file>, objects must be built with /Gw.
   ARM64EC data definitions become references that /arm64xcrossresolve binds to the ARM64 copy. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#define SCN_CNT_CODE        0x00000020
#define SCN_LNK_NRELOC_OVFL 0x01000000
#define SCN_MEM_WRITE       0x80000000

#define SYM_CLASS_EXTERNAL  2
#define SYM_CLASS_STATIC    3

#define FILE_HEADER_SIZE    20
#define SECTION_SIZE        40
#define SYMBOL_SIZE         18
#define RELOC_SIZE          10

typedef struct _OBJ_FILE
{
    uint8_t *Data;
    size_t Size;
    uint16_t NumberOfSections;
    uint32_t SymbolTable;
    uint32_t NumberOfSymbols;
    uint8_t *Strings;
    uint32_t StringsSize;
} OBJ_FILE;

static uint16_t Get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t Get32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static void Put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void Put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static uint8_t *SectionHeader(OBJ_FILE *Obj, int Number)
{
    uint16_t OptSize = Get16(Obj->Data + 16);
    return Obj->Data + FILE_HEADER_SIZE + OptSize + (Number - 1) * SECTION_SIZE;
}

static uint8_t *Symbol(OBJ_FILE *Obj, uint32_t Index)
{
    return Obj->Data + Obj->SymbolTable + Index * SYMBOL_SIZE;
}

/* Copies the symbol name, long names live in the string table */
static void SymbolName(OBJ_FILE *Obj, const uint8_t *Sym, char *Out, size_t OutSize)
{
    if (Get32(Sym) == 0)
    {
        uint32_t Offset = Get32(Sym + 4);
        snprintf(Out, OutSize, "%s", (const char *)Obj->Strings + Offset);
    }
    else
    {
        size_t Len = 0;
        while (Len < 8 && Sym[Len]) Len++;
        if (Len >= OutSize) Len = OutSize - 1;
        memcpy(Out, Sym, Len);
        Out[Len] = 0;
    }
}

/* Appends a name to the string table and returns its offset */
static uint32_t AddString(OBJ_FILE *Obj, const char *Name)
{
    size_t Len = strlen(Name) + 1;
    uint32_t Offset = Obj->StringsSize;
    Obj->Strings = realloc(Obj->Strings, Obj->StringsSize + Len);
    if (!Obj->Strings)
    {
        fprintf(stderr, "arm64xobj: out of memory\n");
        exit(1);
    }
    memcpy(Obj->Strings + Obj->StringsSize, Name, Len);
    Obj->StringsSize += (uint32_t)Len;
    return Offset;
}

static void SetSymbolName(OBJ_FILE *Obj, uint8_t *Sym, const char *Name)
{
    Put32(Sym, 0);
    Put32(Sym + 4, AddString(Obj, Name));
}

/* FNV-1a over the normalized source path, the same for both builds */
static uint32_t PathTag(const char *Path)
{
    uint32_t Hash = 2166136261u;
    for (; *Path; Path++)
    {
        int c = tolower((unsigned char)*Path);
        if (c == '\\') c = '/';
        Hash = (Hash ^ (uint32_t)c) * 16777619u;
    }
    return Hash;
}

static int IsWritableData(OBJ_FILE *Obj, int16_t Section)
{
    uint32_t Flags;
    char Name[9] = { 0 };

    if (Section <= 0 || Section > Obj->NumberOfSections)
        return 0;

    memcpy(Name, SectionHeader(Obj, Section), 8);
    Flags = Get32(SectionHeader(Obj, Section) + 36);

    /* Thread local data stays per view, it is laid out by the TLS directory */
    if (!strncmp(Name, ".tls", 4))
        return 0;

    return (Flags & SCN_MEM_WRITE) && !(Flags & SCN_CNT_CODE);
}

/* Tells whether the object exports a symbol with a linker directive, the export needs this half's definition */
static int IsExported(OBJ_FILE *Obj, const char *Name)
{
    size_t NameLength = strlen(Name);
    int s;

    for (s = 1; s <= Obj->NumberOfSections; s++)
    {
        uint8_t *Header = SectionHeader(Obj, s);
        const char *Data = (const char *)Obj->Data + Get32(Header + 20);
        uint32_t Size = Get32(Header + 16), i;

        if (memcmp(Header, ".drectve", 8) || Get32(Header + 20) + Size > Obj->Size)
            continue;

        for (i = 0; i + 8 + NameLength <= Size; i++)
        {
            if ((Data[i] == '/' || Data[i] == '-') && !_strnicmp(Data + i + 1, "EXPORT:", 7) &&
                !memcmp(Data + i + 8, Name, NameLength) &&
                (i + 8 + NameLength == Size || strchr(", \"", Data[i + 8 + NameLength])))
            {
                return 1;
            }
        }
    }

    return 0;
}

/* Function pointers differ between the halves, each half keeps data that holds them */
static int HoldsCodePointers(OBJ_FILE *Obj, int16_t Section)
{
    uint8_t *Header = SectionHeader(Obj, Section);
    uint32_t Relocs = Get32(Header + 24);
    uint32_t Count = Get16(Header + 32);
    uint32_t r;

    if (Get32(Header + 36) & SCN_LNK_NRELOC_OVFL)
        Count = Get32(Obj->Data + Relocs + 4);

    for (r = 0; r < Count; r++)
    {
        uint32_t Index = Get32(Obj->Data + Relocs + r * RELOC_SIZE + 4);
        uint8_t *Target;
        int16_t TargetSection;

        if (Index >= Obj->NumberOfSymbols)
            continue;

        Target = Symbol(Obj, Index);
        TargetSection = (int16_t)Get16(Target + 12);

        /* The function type, or a definition in a code section */
        if ((Get16(Target + 14) & 0xF0) == 0x20)
            return 1;
        if (TargetSection > 0 && TargetSection <= Obj->NumberOfSections &&
            (Get32(SectionHeader(Obj, TargetSection) + 36) & SCN_CNT_CODE))
        {
            return 1;
        }
    }

    return 0;
}

static int LoadObject(const char *Path, OBJ_FILE *Obj)
{
    FILE *File = fopen(Path, "rb");
    uint32_t StringsOffset;

    if (!File)
        return 0;
    fseek(File, 0, SEEK_END);
    Obj->Size = (size_t)ftell(File);
    fseek(File, 0, SEEK_SET);
    Obj->Data = malloc(Obj->Size);
    if (!Obj->Data || fread(Obj->Data, 1, Obj->Size, File) != Obj->Size)
    {
        fclose(File);
        return 0;
    }
    fclose(File);

    /* Anonymous objects (bigobj, LTCG) are left alone */
    if (Get16(Obj->Data) == 0 && Get16(Obj->Data + 2) == 0xFFFF)
        return -1;

    Obj->NumberOfSections = Get16(Obj->Data + 2);
    Obj->SymbolTable = Get32(Obj->Data + 8);
    Obj->NumberOfSymbols = Get32(Obj->Data + 12);
    if (!Obj->SymbolTable)
        return -1;

    StringsOffset = Obj->SymbolTable + Obj->NumberOfSymbols * SYMBOL_SIZE;
    Obj->StringsSize = Get32(Obj->Data + StringsOffset);
    Obj->Strings = malloc(Obj->StringsSize);
    memcpy(Obj->Strings, Obj->Data + StringsOffset, Obj->StringsSize);
    return 1;
}

static int SaveObject(const char *Path, OBJ_FILE *Obj, const uint8_t *NewSymbols, uint32_t NewCount)
{
    FILE *File = fopen(Path, "wb");
    uint32_t Total = Obj->NumberOfSymbols + NewCount;
    uint8_t Size[4];

    if (!File)
        return 0;

    Put32(Obj->Data + 12, Total);
    fwrite(Obj->Data, 1, Obj->SymbolTable + Obj->NumberOfSymbols * SYMBOL_SIZE, File);
    if (NewCount)
        fwrite(NewSymbols, SYMBOL_SIZE, NewCount, File);
    Put32(Size, Obj->StringsSize);
    fwrite(Size, 1, 4, File);
    fwrite(Obj->Strings + 4, 1, Obj->StringsSize - 4, File);
    fclose(File);
    return 1;
}

static int RewriteObject(int Ec, const char *Source, const char *Object)
{
    OBJ_FILE Obj = { 0 };
    int Result;
    uint32_t Tag, i, NewCount = 0;
    uint32_t *Redirect;
    uint8_t *NewSymbols;
    char Name[512], NewName[600];

    Tag = PathTag(Source);

    Result = LoadObject(Object, &Obj);
    if (Result == 0)
    {
        fprintf(stderr, "arm64xobj: cannot read %s\n", Object);
        return 1;
    }
    if (Result < 0)
        return 0;

    Redirect = calloc(Obj.NumberOfSymbols, sizeof(*Redirect));
    NewSymbols = calloc(Obj.NumberOfSymbols, SYMBOL_SIZE);
    if (!Redirect || !NewSymbols)
    {
        fprintf(stderr, "arm64xobj: out of memory\n");
        return 1;
    }

    for (i = 0; i < Obj.NumberOfSymbols; i += 1 + Symbol(&Obj, i)[17])
    {
        uint8_t *Sym = Symbol(&Obj, i);
        int16_t Section = (int16_t)Get16(Sym + 12);
        uint8_t Class = Sym[16];
        uint8_t AuxCount = Sym[17];
        int IsCommon = Class == SYM_CLASS_EXTERNAL && Section == 0 && Get32(Sym + 8) != 0;

        Redirect[i] = i;

        /* The PCH marker embeds the build path, each half keeps its own */
        if (Get32(Sym) == 0 && !strncmp((const char *)Obj.Strings + Get32(Sym + 4), "__@@_PchSym_", 12))
            continue;

        /* Common data is shared like defined data, the ARM64 half allocates it */
        if (!IsCommon)
        {
            /* Section symbols carry an aux record, data items do not */
            if (AuxCount || !IsWritableData(&Obj, Section) || HoldsCodePointers(&Obj, Section))
                continue;
            if (Class != SYM_CLASS_EXTERNAL && Class != SYM_CLASS_STATIC)
                continue;
        }

        SymbolName(&Obj, Sym, Name, sizeof(Name));

        /* Data the object exports itself keeps its definition in each half */
        if (Ec && Class == SYM_CLASS_EXTERNAL && IsExported(&Obj, Name))
            continue;

        /* File local data gets a name both halves agree on */
        if (Class == SYM_CLASS_STATIC)
        {
            snprintf(NewName, sizeof(NewName), "%s@x%08x", Name, Tag);
            if (!Ec)
            {
                SetSymbolName(&Obj, Sym, NewName);
                Sym[16] = SYM_CLASS_EXTERNAL;
                continue;
            }
        }
        else
        {
            snprintf(NewName, sizeof(NewName), "%s", Name);
            if (!Ec)
                continue;
        }

        /* The definition stays as a dead local so its COMDAT keeps a leader, users move to an undefined twin */
        {
            uint8_t *Twin = NewSymbols + NewCount * SYMBOL_SIZE;
            char Dead[sizeof(Name) + 16];

            memset(Twin, 0, SYMBOL_SIZE);
            SetSymbolName(&Obj, Twin, NewName);
            Twin[16] = SYM_CLASS_EXTERNAL;
            Redirect[i] = Obj.NumberOfSymbols + NewCount;
            NewCount++;

            /* An exported item still needs this copy, it holds the ARM64EC pointers */
            if (Class == SYM_CLASS_EXTERNAL)
            {
                snprintf(Dead, sizeof(Dead), "%s$arm64ec", Name);
            }
            else
            {
                snprintf(Dead, sizeof(Dead), "$arm64x$%u", i);
                Sym[16] = SYM_CLASS_STATIC;
            }
            SetSymbolName(&Obj, Sym, Dead);
        }
    }

    /* Point every relocation at the twins */
    if (NewCount)
    {
        int s;
        for (s = 1; s <= Obj.NumberOfSections; s++)
        {
            uint8_t *Header = SectionHeader(&Obj, s);
            uint32_t Relocs = Get32(Header + 24);
            uint32_t Count = Get16(Header + 32);
            uint32_t r;

            /* The real count is in the first entry when it does not fit */
            if ((Get32(Header + 36) & SCN_LNK_NRELOC_OVFL) && Count == 0xFFFF)
                Count = Get32(Obj.Data + Relocs);

            for (r = 0; r < Count; r++)
            {
                uint8_t *Reloc = Obj.Data + Relocs + r * RELOC_SIZE;
                uint32_t Index = Get32(Reloc + 4);
                if (Index < Obj.NumberOfSymbols && Redirect[Index] != Index)
                    Put32(Reloc + 4, Redirect[Index]);
            }
        }
    }

    if (!SaveObject(Object, &Obj, NewSymbols, NewCount))
    {
        fprintf(stderr, "arm64xobj: cannot write %s\n", Object);
        return 1;
    }

    free(Obj.Data);
    free(Obj.Strings);
    free(Redirect);
    free(NewSymbols);
    return 0;
}

#ifdef _WIN32
#include <windows.h>

typedef struct _ARG_LIST
{
    char **Items;
    int Count;
    int Capacity;
} ARG_LIST;

static void AddArg(ARG_LIST *List, const char *Arg)
{
    if (List->Count == List->Capacity)
    {
        List->Capacity = List->Capacity ? List->Capacity * 2 : 64;
        List->Items = realloc(List->Items, List->Capacity * sizeof(*List->Items));
        if (!List->Items)
        {
            fprintf(stderr, "arm64xobj: out of memory\n");
            exit(1);
        }
    }
    List->Items[List->Count++] = _strdup(Arg);
}

/* Quotes an argument the way the MSVC runtime splits command lines */
static void AppendQuoted(char **Line, size_t *Length, const char *Arg)
{
    size_t Needed = *Length + strlen(Arg) * 2 + 4;
    char *Out;
    int Quote = !*Arg || strpbrk(Arg, " \t\"") != NULL;
    const char *p;

    *Line = realloc(*Line, Needed);
    Out = *Line + *Length;
    if (*Length)
        *Out++ = ' ';
    if (Quote)
        *Out++ = '"';
    for (p = Arg; *p; p++)
    {
        size_t Slashes = 0;
        while (p[Slashes] == '\\') Slashes++;
        if (p[Slashes] == '"' || (Quote && !p[Slashes]))
        {
            size_t k;
            for (k = 0; k < Slashes * 2; k++) *Out++ = '\\';
            p += Slashes;
            if (!*p)
                break;
            *Out++ = '\\';
            *Out++ = '"';
            continue;
        }
        *Out++ = *p;
    }
    if (Quote)
        *Out++ = '"';
    *Out = 0;
    *Length = (size_t)(Out - *Line);
}

/* Builds the command line, past the Windows limit the arguments go to a response file the caller deletes */
static char *BuildCommandLine(ARG_LIST *Args, char *ResponseFile, size_t ResponseFileSize)
{
    char *Line = NULL, *Rest = NULL, Dir[MAX_PATH];
    size_t Length = 0, RestLength = 0;
    FILE *File;
    int i;

    *ResponseFile = 0;
    for (i = 0; i < Args->Count; i++)
        AppendQuoted(&Line, &Length, Args->Items[i]);
    if (Length < 30000)
        return Line;

    for (i = 1; i < Args->Count; i++)
        AppendQuoted(&Rest, &RestLength, Args->Items[i]);
    if (!GetTempPathA(sizeof(Dir), Dir) || !GetTempFileNameA(Dir, "a64", 0, ResponseFile) ||
        ResponseFileSize < MAX_PATH || !(File = fopen(ResponseFile, "wb")))
    {
        free(Rest);
        return Line;
    }
    fwrite(Rest, 1, RestLength, File);
    fclose(File);
    free(Rest);
    free(Line);

    Line = NULL;
    Length = 0;
    AppendQuoted(&Line, &Length, Args->Items[0]);
    Line = realloc(Line, Length + strlen(ResponseFile) + 4);
    sprintf(Line + Length, " @%s", ResponseFile);
    return Line;
}

static int RunCommand(ARG_LIST *Args)
{
    STARTUPINFOA StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    char ResponseFile[MAX_PATH], *Line;
    DWORD ExitCode = 1;

    Line = BuildCommandLine(Args, ResponseFile, sizeof(ResponseFile));
    if (!CreateProcessA(NULL, Line, NULL, NULL, TRUE, 0, NULL, NULL, &StartupInfo, &ProcessInfo))
    {
        fprintf(stderr, "arm64xobj: cannot run %s (%lu)\n", Args->Items[0], GetLastError());
        free(Line);
        return 1;
    }

    WaitForSingleObject(ProcessInfo.hProcess, INFINITE);
    GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode);
    CloseHandle(ProcessInfo.hProcess);
    CloseHandle(ProcessInfo.hThread);
    if (*ResponseFile)
        DeleteFileA(ResponseFile);
    free(Line);
    return (int)ExitCode;
}

/* Runs a command like RunCommand, passing its output on and keeping a copy */
static int RunCaptured(ARG_LIST *Args, char **Output)
{
    SECURITY_ATTRIBUTES Security = { sizeof(Security), NULL, TRUE };
    STARTUPINFOA StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    HANDLE Read, Write;
    char ResponseFile[MAX_PATH], *Line, Buffer[4096], *Grown;
    size_t Used = 0;
    DWORD ExitCode = 1, Got;

    *Output = calloc(1, 1);
    if (!*Output || !CreatePipe(&Read, &Write, &Security, 0))
        return RunCommand(Args);
    SetHandleInformation(Read, HANDLE_FLAG_INHERIT, 0);

    Line = BuildCommandLine(Args, ResponseFile, sizeof(ResponseFile));

    StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    StartupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    StartupInfo.hStdOutput = Write;
    StartupInfo.hStdError = Write;

    if (!CreateProcessA(NULL, Line, NULL, NULL, TRUE, 0, NULL, NULL, &StartupInfo, &ProcessInfo))
    {
        fprintf(stderr, "arm64xobj: cannot run %s (%lu)\n", Args->Items[0], GetLastError());
        CloseHandle(Read);
        CloseHandle(Write);
        free(Line);
        return 1;
    }
    CloseHandle(Write);

    while (ReadFile(Read, Buffer, sizeof(Buffer), &Got, NULL) && Got)
    {
        fwrite(Buffer, 1, Got, stdout);
        Grown = realloc(*Output, Used + Got + 1);
        if (Grown)
        {
            *Output = Grown;
            memcpy(*Output + Used, Buffer, Got);
            Used += Got;
            (*Output)[Used] = 0;
        }
    }
    fflush(stdout);

    WaitForSingleObject(ProcessInfo.hProcess, INFINITE);
    GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode);
    CloseHandle(ProcessInfo.hProcess);
    CloseHandle(ProcessInfo.hThread);
    CloseHandle(Read);
    if (*ResponseFile)
        DeleteFileA(ResponseFile);
    free(Line);
    return (int)ExitCode;
}

static int NativePath(const char *Path, const char *EcRoot, const char *NativeRoot, char *Out, size_t OutSize);
static void MapToNative(const char *Path, const char *EcRoot, const char *NativeRoot, char *Out, size_t OutSize);

/* Compiler launcher: runs the compile, then rewrites the object it produced.
   ARM64EC objects keep their data unless an image links their ARM64 twin directly. A twin
   inside a static library may never be linked, nothing would define the data then. */
static int CompileMode(int Ec, const char *EcRoot, const char *NativeRoot, int argc, char *argv[])
{
    ARG_LIST Args = { 0 };
    const char *Object = NULL, *Source = NULL;
    char Native[MAX_PATH + 32], NativeSource[MAX_PATH + 32];
    int i, Result;

    for (i = 0; i < argc; i++)
    {
        AddArg(&Args, argv[i]);
        if ((argv[i][0] == '/' || argv[i][0] == '-') && !strncmp(argv[i] + 1, "Fo", 2))
            Object = argv[i] + 3;
    }
    Source = argv[argc - 1];

    Result = RunCommand(&Args);
    if (Result || !Object)
        return Result;

    if (Ec)
    {
        if (!NativePath(Object, EcRoot, NativeRoot, Native, sizeof(Native)))
            return 0;

        strcat(Native, ".arm64xd");
        if (GetFileAttributesA(Native) == INVALID_FILE_ATTRIBUTES)
            return 0;

        /* Generated sources live in each build tree, name statics after the ARM64 copy */
        MapToNative(Source, EcRoot, NativeRoot, NativeSource, sizeof(NativeSource));
        Source = NativeSource;
    }

    return RewriteObject(Ec, Source, Object);
}

/* Splits a response file into arguments */
static void ReadResponseFile(ARG_LIST *Args, const char *Path)
{
    FILE *File = fopen(Path, "rb");
    char Token[4096];
    size_t Len = 0;
    int c, InQuote = 0, HaveToken = 0;

    if (!File)
    {
        fprintf(stderr, "arm64xobj: cannot read %s\n", Path);
        exit(1);
    }

    while ((c = fgetc(File)) != EOF)
    {
        if (c == '"')
        {
            InQuote = !InQuote;
            HaveToken = 1;
            continue;
        }
        if (!InQuote && (c == ' ' || c == '\t' || c == '\r' || c == '\n'))
        {
            if (HaveToken)
            {
                Token[Len] = 0;
                AddArg(Args, Token);
                Len = 0;
                HaveToken = 0;
            }
            continue;
        }
        if (Len < sizeof(Token) - 1)
            Token[Len++] = (char)c;
        HaveToken = 1;
    }
    if (HaveToken)
    {
        Token[Len] = 0;
        AddArg(Args, Token);
    }
    fclose(File);
}

static int EndsWith(const char *Str, const char *Suffix)
{
    size_t a = strlen(Str), b = strlen(Suffix);
    return a >= b && !_stricmp(Str + a - b, Suffix);
}

/* Maps a path in the ARM64EC tree to the same path in the ARM64 tree, if it exists there */
static int NativePath(const char *Path, const char *EcRoot, const char *NativeRoot, char *Out, size_t OutSize)
{
    char Full[MAX_PATH], Root[MAX_PATH];
    size_t RootLen;

    if (!GetFullPathNameA(Path, sizeof(Full), Full, NULL) ||
        !GetFullPathNameA(EcRoot, sizeof(Root), Root, NULL))
    {
        return 0;
    }

    RootLen = strlen(Root);
    while (RootLen && (Root[RootLen - 1] == '\\' || Root[RootLen - 1] == '/'))
        Root[--RootLen] = 0;

    if (_strnicmp(Full, Root, RootLen) || (Full[RootLen] != '\\' && Full[RootLen] != '/'))
        return 0;

    snprintf(Out, OutSize, "%s%s", NativeRoot, Full + RootLen);
    return GetFileAttributesA(Out) != INVALID_FILE_ATTRIBUTES;
}

/* The ARM64 build's name of a path inside the ARM64EC build, other paths stay as they are */
static void MapToNative(const char *Path, const char *EcRoot, const char *NativeRoot, char *Out, size_t OutSize)
{
    if (!NativePath(Path, EcRoot, NativeRoot, Out, OutSize))
        snprintf(Out, OutSize, "%s", Path);
}

static void ExpandArgs(ARG_LIST *List, int argc, char *argv[])
{
    int i;
    for (i = 0; i < argc; i++)
    {
        if (argv[i][0] == '@')
            ReadResponseFile(List, argv[i] + 1);
        else
            AddArg(List, argv[i]);
    }
}

static int IsOption(const char *Arg, const char *Name)
{
    return (Arg[0] == '/' || Arg[0] == '-') && !_strnicmp(Arg + 1, Name, strlen(Name));
}

static const char *FindOutput(ARG_LIST *List)
{
    int i;
    for (i = 1; i < List->Count; i++)
    {
        if (IsOption(List->Items[i], "out:"))
            return List->Items[i] + 5;
    }
    return NULL;
}

static int IsInput(const char *Arg)
{
    return Arg[0] != '/' && Arg[0] != '-' && (EndsWith(Arg, ".obj") || EndsWith(Arg, ".lib"));
}

/* Tells whether a library holds a symbol, names in its symbol tables end with a NUL */
static int LibraryHasSymbol(const char *Library, const char *Name)
{
    FILE *File = fopen(Library, "rb");
    size_t Length, NameLength = strlen(Name) + 1, i;
    char *Data;
    int Found = 0;

    if (!File)
        return 0;

    fseek(File, 0, SEEK_END);
    Length = (size_t)ftell(File);
    fseek(File, 0, SEEK_SET);
    Data = malloc(Length + 1);
    if (Data && fread(Data, 1, Length, File) == Length)
    {
        for (i = 1; i + NameLength <= Length && !Found; i++)
            Found = !Data[i - 1] && !memcmp(Data + i, Name, NameLength);
    }
    free(Data);
    fclose(File);
    return Found;
}

/* The import library an ARM64EC link uses for a module, if it is one of the inputs */
static const char *FindImportLibrary(ARG_LIST *List, const char *Module, size_t ModuleLength)
{
    char Name[MAX_PATH];
    const char *Base;
    int i;

    snprintf(Name, sizeof(Name), "lib%.*s.lib", (int)ModuleLength, Module);
    for (i = 1; i < List->Count; i++)
    {
        if (!IsInput(List->Items[i]))
            continue;

        Base = strrchr(List->Items[i], '\\');
        if (!Base || (strrchr(List->Items[i], '/') > Base))
            Base = strrchr(List->Items[i], '/');
        Base = Base ? Base + 1 : List->Items[i];

        if (!_stricmp(Base, Name))
            return List->Items[i];
    }
    return NULL;
}

/* Collects the names of the ARM64EC copies of shared data in the objects of a link, each as \x01name */
static char *CollectEcCopies(ARG_LIST *List)
{
    size_t Size = 1, Used = 0, Length, i;
    char *Names = calloc(1, 1), *Data, *Grown;
    FILE *File;
    int n;

    for (n = 1; n < List->Count && Names; n++)
    {
        if (!EndsWith(List->Items[n], ".obj") || !(File = fopen(List->Items[n], "rb")))
            continue;

        fseek(File, 0, SEEK_END);
        Length = (size_t)ftell(File);
        fseek(File, 0, SEEK_SET);
        Data = malloc(Length + 1);
        if (Data && fread(Data, 1, Length, File) == Length)
        {
            Data[Length] = 0;
            for (i = 8; i + 1 < Length; i++)
            {
                /* Names in the string table end with the suffix and a NUL */
                if (Data[i] == 0 && !memcmp(Data + i - 8, "$arm64ec", 8))
                {
                    size_t Start = i - 8;

                    while (Start > 0 && Data[Start - 1] > ' ')
                        Start--;
                    Grown = realloc(Names, Size + (i - Start) + 2);
                    if (!Grown)
                        break;
                    Names = Grown;
                    Names[Used] = 0x01;
                    memcpy(Names + Used + 1, Data + Start, i - Start);
                    Used += i - Start + 1;
                    Names[Used] = 0;
                    Size = Used + 1;
                }
            }
        }
        free(Data);
        fclose(File);
    }

    /* A trailing separator lets lookups match whole names */
    if (Names && (Grown = realloc(Names, Size + 1)))
    {
        Names = Grown;
        Names[Used] = 0x01;
        Names[Used + 1] = 0;
    }
    return Names;
}

/* Tells whether the ARM64EC objects of the link call a function, by its ARM64EC name */
static int ObjectsReference(ARG_LIST *List, const char *Name, size_t Length)
{
    char Symbol[512];
    int i;

    snprintf(Symbol, sizeof(Symbol), "#%.*s", (int)Length, Name);
    for (i = 1; i < List->Count; i++)
    {
        if (EndsWith(List->Items[i], ".obj") && LibraryHasSymbol(List->Items[i], Symbol))
            return 1;
    }

    return 0;
}

/* An ARM64EC export that forwards to a function the image imports takes that function's ARM64EC
   import thunk away in an ARM64X link. Exporting the import itself keeps the thunk, and x64
   callers of the export still end up in the same function. */
static int RewriteForwarders(ARG_LIST *List, const char *Def, const char *Output, const char *Keep, char *NewDef, size_t NewDefSize, int *ExportsHandler)
{
    char Line[1024], Symbol[512];
    const char *Name, *Equal, *Dot, *Target, *End, *Library;
    FILE *In, *Out;
    int IsHandler;
    char *EcCopies;

    In = fopen(Def, "r");
    if (!In)
        return 0;

    snprintf(NewDef, NewDefSize, "%s.ec.def", Output);
    Out = fopen(NewDef, "w");
    if (!Out)
    {
        fclose(In);
        return 0;
    }

    EcCopies = CollectEcCopies(List);

    while (fgets(Line, sizeof(Line), In))
    {
        Name = Line;
        while (*Name == ' ' || *Name == '\t')
            Name++;

        IsHandler = !strncmp(Name, "__C_specific_handler", 20) && strchr("= \t\r\n", Name[20]);
        if (IsHandler)
            *ExportsHandler = 1;

        Equal = strchr(Name, '=');
        Dot = Equal ? strchr(Equal, '.') : NULL;
        Target = Dot ? Dot + 1 : NULL;
        End = Target ? Target + strcspn(Target, " \t\r\n") : NULL;

        if (Target && End > Target && *Target != '#' && !memchr(Name, ' ', Equal - Name))
        {
            Library = FindImportLibrary(List, Equal + 1, Dot - Equal - 1);
            snprintf(Symbol, sizeof(Symbol), "%c%.*s%c", 0x02, (int)(End - Target), Target, 0x02);
            if (Keep && strstr(Keep, Symbol))
                continue;

            snprintf(Symbol, sizeof(Symbol), "%c%.*s%c", 0x01, (int)(End - Target), Target, 0x01);
            if (Keep && strstr(Keep, Symbol))
                Library = NULL;

            snprintf(Symbol, sizeof(Symbol), "__imp_%.*s", (int)(End - Target), Target);
            if (Library && LibraryHasSymbol(Library, Symbol) && ObjectsReference(List, Target, End - Target))
            {
                int SameName = (size_t)(End - Target) == (size_t)(Equal - Name) && !memcmp(Name, Target, End - Target);

                /* A forwarder to the same name exports the import, or the local definition for the handler */
                if (SameName)
                    fprintf(Out, "%.*s%s", (int)(Equal - Line), Line, End);
                else
                    fprintf(Out, "%.*s%s", (int)(Equal + 1 - Line), Line, Target);
                continue;
            }
        }
        /* Exported data the ARM64 half shares keeps its ARM64EC copy under another name */
        if (!Equal && EcCopies)
        {
            size_t NameLength = strcspn(Name, " \t\r\n");

            snprintf(Symbol, sizeof(Symbol), "%c%.*s$arm64ec%c", 0x01, (int)NameLength, Name, 0x01);
            if (strstr(EcCopies, Symbol))
            {
                Symbol[strlen(Symbol) - 1] = 0;
                memmove(Symbol, Symbol + 1, strlen(Symbol));
                fprintf(Out, "%.*s=%s%s", (int)(Name + NameLength - Line), Line, Symbol, Name + NameLength);
                continue;
            }
        }
        fputs(Line, Out);
    }

    free(EcCopies);
    fclose(In);
    fclose(Out);
    return 1;
}

/* Marks an ARM64 object that a sharing image links directly, its ARM64EC twin may then use its data */
static void MarkLinked(const char *Object)
{
    char Path[MAX_PATH + 16];
    FILE *File;

    snprintf(Path, sizeof(Path), "%s.arm64xd", Object);
    File = fopen(Path, "w");
    if (File)
        fclose(File);
}

/* Linker launcher for the ARM64 build: keeps the inputs of each image for its ARM64X link */
static int RecordMode(int argc, char *argv[])
{
    ARG_LIST Args = { 0 }, In = { 0 };
    const char *Output;
    char Path[MAX_PATH + 16], Full[MAX_PATH];
    FILE *File;
    int i, IsDll = 0, Share = 0;

    /* /arm64xshare is ours: the image wants its data shared by both halves, the linker never sees it */
    ExpandArgs(&Args, argc, argv);
    for (i = 0; i < Args.Count; i++)
    {
        if (i && IsOption(Args.Items[i], "arm64xshare"))
        {
            Share = 1;
            continue;
        }
        if (i && IsOption(Args.Items[i], "dll"))
            IsDll = 1;
        AddArg(&In, Args.Items[i]);
    }

    /* Only DLLs become ARM64X images, programs stay ARM64 */
    Output = FindOutput(&In);
    if (Output && IsDll)
    {
        snprintf(Path, sizeof(Path), "%s.arm64x", Output);
        File = fopen(Path, "w");
        if (File)
        {
            for (i = 1; i < In.Count; i++)
            {
                const char *Arg = In.Items[i];
                if (IsInput(Arg) && GetFullPathNameA(Arg, sizeof(Full), Full, NULL))
                {
                    fprintf(File, "%s\n", Full);
                    if (Share && EndsWith(Full, ".obj"))
                        MarkLinked(Full);
                }
                else if (IsOption(Arg, "DEF:") && GetFullPathNameA(Arg + 5, sizeof(Full), Full, NULL))
                    fprintf(File, "/defarm64native:%s\n", Full);
            }
            fclose(File);
        }
    }

    return RunCommand(&In);
}

/* Linker launcher for the ARM64EC build: links DLLs as ARM64X with the ARM64 inputs the native build recorded */
static int LinkMode(const char *EcRoot, const char *NativeRoot, int argc, char *argv[])
{
    ARG_LIST In = { 0 }, Out = { 0 };
    char Native[MAX_PATH + 32], Line[MAX_PATH + 64], NewDef[MAX_PATH + 16];
    char *Keep = NULL, *Log = NULL, *Grown;
    const char *Output, *Def = NULL, *Hit;
    FILE *Record = NULL;
    int i, IsDll = 0, ExportsHandler = 0, Result, Added;

    ExpandArgs(&In, argc, argv);

    for (i = 1; i < In.Count; i++)
    {
        if (IsOption(In.Items[i], "dll"))
            IsDll = 1;
    }

    Output = FindOutput(&In);
    if (IsDll && Output && NativePath(Output, EcRoot, NativeRoot, Native, sizeof(Native)))
    {
        strcat(Native, ".arm64x");
        Record = fopen(Native, "r");
    }

    /* Without the ARM64 half this stays a plain ARM64EC link */
    if (!Record)
        return RunCommand(&In);

    for (i = 0; i < In.Count; i++)
    {
        if (IsOption(In.Items[i], "machine:"))
        {
            AddArg(&Out, "/machine:arm64x");
            AddArg(&Out, "/arm64xcrossresolve");
            AddArg(&Out, "/ignore:4293");
            continue;
        }
        if (IsOption(In.Items[i], "DEF:") &&
            RewriteForwarders(&In, In.Items[i] + 5, Output, NULL, NewDef, sizeof(NewDef), &ExportsHandler))
        {
            Def = In.Items[i] + 5;
            snprintf(Line, sizeof(Line), "/DEF:%s", NewDef);
            AddArg(&Out, Line);

            /* Import libraries define the handler too, an exported one has to be the local definition */
            if (ExportsHandler)
                AddArg(&Out, "/INCLUDE:#__C_specific_handler");
            continue;
        }
        AddArg(&Out, In.Items[i]);
    }

    while (fgets(Line, sizeof(Line), Record))
    {
        Line[strcspn(Line, "\r\n")] = 0;
        if (*Line)
            AddArg(&Out, Line);
    }
    fclose(Record);

    /* The ARM64 half needs its own load config, plain ARM64 images do not carry one */
    snprintf(Line, sizeof(Line), "%s/sdk/lib/hybrid/hybrid.lib", NativeRoot);
    if (GetFileAttributesA(Line) != INVALID_FILE_ATTRIBUTES)
        AddArg(&Out, Line);

    /* An exported handler is the local one, import libraries searched before it would supply theirs */
    if (ExportsHandler)
    {
        ARG_LIST Ordered = { 0 };
        const char *Handler = NULL;
        int Placed = 0;

        for (i = 1; i < Out.Count; i++)
        {
            if (EndsWith(Out.Items[i], "ehandler.lib"))
                Handler = Out.Items[i];
        }
        for (i = 0; i < Out.Count; i++)
        {
            if (Handler && !Placed && i && EndsWith(Out.Items[i], ".lib") && IsInput(Out.Items[i]))
            {
                AddArg(&Ordered, Handler);
                Placed = 1;
            }
            AddArg(&Ordered, Out.Items[i]);
        }
        Out = Ordered;
    }

    /* An export of an import that ARM64EC code reaches without an exit thunk fails the link, those stay forwarders */
    for (;;)
    {
        free(Log);
        Result = RunCaptured(&Out, &Log);
        Hit = (Result && Def && Log) ? strstr(Log, "LNK1413: ARM64EC symbol '#") : NULL;

        for (Added = 0; Hit; Hit = strstr(Hit, "LNK1413: ARM64EC symbol '#"))
        {
            Hit += strlen("LNK1413: ARM64EC symbol '#");
            Grown = realloc(Keep, (Keep ? strlen(Keep) : 0) + strcspn(Hit, "'") + 3);
            if (!Grown)
                break;
            if (!Keep)
                Grown[0] = 0;
            Keep = Grown;

            /* A forwarder that fails as well leaves the x64 view, the native view still exports it */
            snprintf(Line, sizeof(Line), "%c%.*s%c", 0x02, (int)strcspn(Hit, "'"), Hit, 0x02);
            if (strstr(Keep, Line))
                continue;

            Line[0] = Line[strlen(Line) - 1] = 0x01;
            if (strstr(Keep, Line))
            {
                Line[0] = Line[strlen(Line) - 1] = 0x02;
                printf("arm64xobj: warning: leaving the export of %.*s out of the x64 view\n", (int)strcspn(Hit, "'"), Hit);
            }
            else
            {
                printf("arm64xobj: keeping the forwarder to %.*s and linking again\n", (int)strcspn(Hit, "'"), Hit);
            }
            strcat(Keep, Line);
            Added = 1;
        }
        if (!Added)
            break;
        RewriteForwarders(&In, Def, Output, Keep, NewDef, sizeof(NewDef), &ExportsHandler);
    }

    free(Log);
    free(Keep);
    return Result;
}
#endif

#ifdef _WIN32
/* Tells whether a preprocessed assembly file is x64 MASM: it has a .code segment and no armasm AREA */
static int IsX64Assembly(const char *Path)
{
    FILE *File = fopen(Path, "rb");
    char Line[1024];
    int Code = 0, Area = 0;
    const char *p;

    if (!File)
        return 0;

    while (fgets(Line, sizeof(Line), File))
    {
        for (p = Line; *p == ' ' || *p == '\t'; p++);
        if (!_strnicmp(p, ".code", 5))
            Code = 1;
        if (!strncmp(p, "AREA", 4) && (p[4] == ' ' || p[4] == '\t'))
            Area = 1;
    }

    fclose(File);
    return Code && !Area;
}

/* Assembler launcher for the ARM64EC build: sources that took the x64 path assemble with ml64 next to armasm64 */
static int AsmMode(int argc, char *argv[])
{
    ARG_LIST Args = { 0 };
    char Ml64[MAX_PATH], Fo[MAX_PATH + 4];
    const char *Object = NULL, *Source = argv[argc - 1];
    char *Slash;
    int i;

    for (i = 0; i < argc; i++)
    {
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            Object = argv[i + 1];
    }

    if (!Object || !IsX64Assembly(Source))
    {
        for (i = 0; i < argc; i++)
            AddArg(&Args, argv[i]);
        return RunCommand(&Args);
    }

    snprintf(Ml64, sizeof(Ml64), "%s", argv[0]);
    Slash = strrchr(Ml64, '\\');
    if (!Slash || strrchr(Ml64, '/') > Slash)
        Slash = strrchr(Ml64, '/');
    snprintf(Slash ? Slash + 1 : Ml64, sizeof(Ml64) - (Slash ? (size_t)(Slash + 1 - Ml64) : 0), "ml64.exe");
    snprintf(Fo, sizeof(Fo), "/Fo%s", Object);

    AddArg(&Args, Ml64);
    AddArg(&Args, "/nologo");
    AddArg(&Args, "/quiet");
    AddArg(&Args, "/Cp");
    AddArg(&Args, Fo);
    AddArg(&Args, "/c");
    AddArg(&Args, "/Ta");
    AddArg(&Args, Source);
    return RunCommand(&Args);
}
#endif

int main(int argc, char *argv[])
{
    if (argc == 4 && (!strcmp(argv[1], "-native") || !strcmp(argv[1], "-ec")))
        return RewriteObject(!strcmp(argv[1], "-ec"), argv[2], argv[3]);

#ifdef _WIN32
    if (argc > 3 && !strcmp(argv[1], "-cc") && !strcmp(argv[2], "-native"))
        return CompileMode(0, NULL, NULL, argc - 3, argv + 3);

    if (argc > 5 && !strcmp(argv[1], "-cc") && !strcmp(argv[2], "-ec"))
        return CompileMode(1, argv[3], argv[4], argc - 5, argv + 5);

    if (argc > 4 && !strcmp(argv[1], "-link"))
        return LinkMode(argv[2], argv[3], argc - 4, argv + 4);

    if (argc > 2 && !strcmp(argv[1], "-record"))
        return RecordMode(argc - 2, argv + 2);

    if (argc > 2 && !strcmp(argv[1], "-asm"))
        return AsmMode(argc - 2, argv + 2);
#endif

    fprintf(stderr,
            "Usage: arm64xobj -native|-ec <source file> <object file>\n"
            "       arm64xobj -cc -native <compiler> <arguments>\n"
            "       arm64xobj -cc -ec <arm64ec tree> <arm64 tree> <compiler> <arguments>\n"
            "       arm64xobj -record <linker> <arguments>\n"
            "       arm64xobj -link <arm64ec tree> <arm64 tree> <linker> <arguments>\n"
            "       arm64xobj -asm <armasm64> <arguments>\n");
    return 1;
}
