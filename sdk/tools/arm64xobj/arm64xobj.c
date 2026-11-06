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

        Redirect[i] = i;

        /* The PCH marker embeds the build path, each half keeps its own */
        if (Get32(Sym) == 0 && !strncmp((const char *)Obj.Strings + Get32(Sym + 4), "__@@_PchSym_", 12))
            continue;

        /* Common data, the ARM64 half allocates it */
        if (Ec && Class == SYM_CLASS_EXTERNAL && Section == 0 && Get32(Sym + 8) != 0)
        {
            Put32(Sym + 8, 0);
            continue;
        }

        /* Section symbols carry an aux record, data items do not */
        if (AuxCount || !IsWritableData(&Obj, Section))
            continue;
        if (Class != SYM_CLASS_EXTERNAL && Class != SYM_CLASS_STATIC)
            continue;

        SymbolName(&Obj, Sym, Name, sizeof(Name));

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
            char Dead[64];

            memset(Twin, 0, SYMBOL_SIZE);
            SetSymbolName(&Obj, Twin, NewName);
            Twin[16] = SYM_CLASS_EXTERNAL;
            Redirect[i] = Obj.NumberOfSymbols + NewCount;
            NewCount++;

            snprintf(Dead, sizeof(Dead), "$arm64x$%u", i);
            SetSymbolName(&Obj, Sym, Dead);
            Sym[16] = SYM_CLASS_STATIC;
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

static int RunCommand(ARG_LIST *Args)
{
    STARTUPINFOA StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    char *Line = NULL;
    size_t Length = 0;
    DWORD ExitCode = 1;
    int i;

    for (i = 0; i < Args->Count; i++)
        AppendQuoted(&Line, &Length, Args->Items[i]);

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
    free(Line);
    return (int)ExitCode;
}

static int NativePath(const char *Path, const char *EcRoot, const char *NativeRoot, char *Out, size_t OutSize);

/* Compiler launcher: runs the compile, then rewrites the object it produced.
   ARM64EC objects without an ARM64 twin keep their data, nothing else would define it. */
static int CompileMode(int Ec, const char *EcRoot, const char *NativeRoot, int argc, char *argv[])
{
    ARG_LIST Args = { 0 };
    const char *Object = NULL, *Source = NULL;
    char Native[MAX_PATH + 32];
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

    if (Ec && !NativePath(Object, EcRoot, NativeRoot, Native, sizeof(Native)))
        return 0;

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

/* Linker launcher for the ARM64 build: keeps the inputs of each image for its ARM64X link */
static int RecordMode(int argc, char *argv[])
{
    ARG_LIST In = { 0 };
    const char *Output;
    char Path[MAX_PATH + 16], Full[MAX_PATH];
    FILE *File;
    int i;

    ExpandArgs(&In, argc, argv);
    Output = FindOutput(&In);
    if (Output)
    {
        snprintf(Path, sizeof(Path), "%s.arm64x", Output);
        File = fopen(Path, "w");
        if (File)
        {
            for (i = 1; i < In.Count; i++)
            {
                const char *Arg = In.Items[i];
                if (IsInput(Arg) && GetFullPathNameA(Arg, sizeof(Full), Full, NULL))
                    fprintf(File, "%s\n", Full);
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
    char Native[MAX_PATH + 32], Line[MAX_PATH + 64];
    const char *Output;
    FILE *Record = NULL;
    int i, IsDll = 0;

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

    return RunCommand(&Out);
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
#endif

    fprintf(stderr,
            "Usage: arm64xobj -native|-ec <source file> <object file>\n"
            "       arm64xobj -cc -native <compiler> <arguments>\n"
            "       arm64xobj -cc -ec <arm64ec tree> <arm64 tree> <compiler> <arguments>\n"
            "       arm64xobj -record <linker> <arguments>\n"
            "       arm64xobj -link <arm64ec tree> <arm64 tree> <linker> <arguments>\n");
    return 1;
}
