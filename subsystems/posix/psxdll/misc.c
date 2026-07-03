/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Time, system information, working directory and identity helpers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

#define PSX_EPERM       1
#define PSX_ENOENT      2
#define PSX_EBADF       9
#define PSX_ENOMEM      12
#define PSX_EINVAL      22
#define PSX_ERANGE      34
#define PSX_ENOSYS      40

#define PSX_F_OK        0

/* Seconds between 1601-01-01 and 1970-01-01 */
#define PSX_EPOCH_DELTA_SECONDS     11644473600LL
#define PSX_EPOCH_DELTA_USECONDS    11644473600000000LL

/* struct utsname: five fixed size fields */
#define PSX_UTSNAME_FIELDS          5
#define PSX_UTSNAME_FIELD_SIZE      14

/* Byte offset of the descriptor in struct _iobuf */
#define PSX_IOBUF_FILE_OFFSET       18

/**
 * @brief Returns elapsed real time in 10ms ticks from the NT system clock.
 * Process times are reported as zero. Masked to stay positive; callers only use deltas.
 */
long
__cdecl
times(
    _Out_writes_bytes_opt_(4 * sizeof(ULONG)) void *TmsBuffer)
{
    LARGE_INTEGER SystemTime;

    if (TmsBuffer != NULL)
        RtlZeroMemory(TmsBuffer, 4 * sizeof(ULONG));

    NtQuerySystemTime(&SystemTime);
    return (long)((ULONG)(SystemTime.QuadPart / 100000) & 0x7FFFFFFF);
}

long
__cdecl
time(
    _Out_opt_ long *Result)
{
    LARGE_INTEGER SystemTime;
    LONGLONG Seconds;

    NtQuerySystemTime(&SystemTime);
    Seconds = (SystemTime.QuadPart / 10000000) - PSX_EPOCH_DELTA_SECONDS;
    if (Result != NULL)
        *Result = (long)Seconds;

    return (long)Seconds;
}

/**
 * @brief Returns the time of day with microsecond resolution. The time zone is
 * reported as zero.
 */
int
__cdecl
gettimeofday(
    _Out_opt_ void *Tv,
    _Out_opt_ void *Tz)
{
    LARGE_INTEGER SystemTime;
    LONGLONG Microseconds;

    if (Tv != NULL)
    {
        NtQuerySystemTime(&SystemTime);
        Microseconds = (SystemTime.QuadPart / 10) - PSX_EPOCH_DELTA_USECONDS;
        ((long *)Tv)[0] = (long)(Microseconds / 1000000);
        ((long *)Tv)[1] = (long)(Microseconds % 1000000);
    }

    if (Tz != NULL)
    {
        ((int *)Tz)[0] = 0;
        ((int *)Tz)[1] = 0;
    }

    return 0;
}

/**
 * @brief Sets the system time. Fails with EPERM without SeSystemtimePrivilege.
 */
int
__cdecl
settimeofday(
    _In_opt_ const void *Tv,
    _In_opt_ const void *Tz)
{
    LARGE_INTEGER SystemTime;
    const long *In = (const long *)Tv;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Tz);

    if (In == NULL || In[1] < 0 || In[1] >= 1000000)
    {
        PsxSetErrno(PSX_EINVAL);
        return -1;
    }

    SystemTime.QuadPart = ((LONGLONG)In[0] + PSX_EPOCH_DELTA_SECONDS) * 10000000 +
                          (LONGLONG)In[1] * 10;
    Status = NtSetSystemTime(&SystemTime, NULL);
    if (!NT_SUCCESS(Status))
    {
        PsxSetErrno(PSX_EPERM);
        return -1;
    }

    return 0;
}

/**
 * @brief Sleeps on the NT delay timer. Sub-100ns remainders round up so the
 * call never returns early.
 */
int
__cdecl
nanosleep(
    _In_opt_ const void *Requested,
    _Out_opt_ void *Remaining)
{
    LARGE_INTEGER Interval;
    const long *Request = (const long *)Requested;

    if (Request == NULL || Request[0] < 0 || Request[1] < 0 || Request[1] >= 1000000000)
    {
        PsxSetErrno(PSX_EINVAL);
        return -1;
    }

    Interval.QuadPart = -((LONGLONG)Request[0] * 10000000 + ((LONGLONG)Request[1] + 99) / 100);
    NtDelayExecution(FALSE, &Interval);

    /* The wait is never interrupted */
    if (Remaining != NULL)
    {
        ((long *)Remaining)[0] = 0;
        ((long *)Remaining)[1] = 0;
    }

    return 0;
}

long
__cdecl
sysconf(
    _In_ int Name)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiSysconf, PSX_BODY_DATALEN(sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)Name;
    return (long)PsxCallServer(&Message);
}

/**
 * @brief Returns the supplementary groups. The server writes List directly
 * into the caller address space.
 */
int
__cdecl
getgroups(
    _In_ int Size,
    _Out_writes_opt_(Size) int *List)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiGetGroups, PSX_BODY_DATALEN(2 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)Size;
    ((PULONG)Message.Data.Raw)[1] = (ULONG)(ULONG_PTR)List;
    return (int)PsxCallServer(&Message);
}

int
__cdecl
uname(
    _Out_writes_bytes_opt_(PSX_UTSNAME_FIELDS * PSX_UTSNAME_FIELD_SIZE) void *Utsname)
{
    static const char *Fields[PSX_UTSNAME_FIELDS] =
    {
        "POSIX", "reactos", "4.0", "1", "x86"
    };
    PCHAR Out = (PCHAR)Utsname;
    const char *Field;
    int i;
    int j;

    if (Utsname == NULL)
    {
        PsxSetErrno(PSX_EINVAL);
        return -1;
    }

    for (i = 0; i < PSX_UTSNAME_FIELDS; i++)
    {
        Field = Fields[i];
        for (j = 0; j < PSX_UTSNAME_FIELD_SIZE - 1 && Field[j] != '\0'; j++)
            Out[i * PSX_UTSNAME_FIELD_SIZE + j] = Field[j];
        Out[i * PSX_UTSNAME_FIELD_SIZE + j] = '\0';
    }

    return 0;
}

/**
 * @brief Converts the NT device path current directory back to a POSIX path.
 * A NULL Buffer allocates the result from the process heap for the caller to free().
 */
char *
__cdecl
getcwd(
    _Out_writes_opt_z_(Size) char *Buffer,
    _In_ unsigned int Size)
{
    CHAR Temp[PSX_PATH_MAX];
    ULONG RootLength = PsxStartupRootLen;
    PCSTR NtPath = PsxStartupCwd;
    ULONG OutLength = 0;
    ULONG Needed;
    ULONG AllocSize;
    ULONG i;

    if (Buffer != NULL && Size == 0)
    {
        PsxSetErrno(PSX_EINVAL);
        return NULL;
    }

    /* Strip the root device prefix when the current directory is under it */
    if (RootLength != 0)
    {
        BOOLEAN Match = TRUE;

        for (i = 0; i < RootLength; i++)
        {
            if (NtPath[i] != PsxStartupRoot[i])
            {
                Match = FALSE;
                break;
            }
        }

        if (Match)
            NtPath += RootLength;
    }

    for (i = 0; NtPath[i] != '\0'; i++)
    {
        if (OutLength + 1 >= sizeof(Temp))
        {
            PsxSetErrno(PSX_ERANGE);
            return NULL;
        }
        Temp[OutLength++] = (NtPath[i] == '\\') ? '/' : NtPath[i];
    }

    /* Drop a trailing separator unless the path is just "/" */
    if (OutLength > 1 && Temp[OutLength - 1] == '/')
        OutLength--;
    if (OutLength == 0)
        Temp[OutLength++] = '/';
    Temp[OutLength] = '\0';
    Needed = OutLength + 1;

    if (Buffer == NULL)
    {
        /* Size 0 means allocate exactly what is needed */
        AllocSize = (Size != 0) ? Size : Needed;
        if (AllocSize < Needed)
        {
            PsxSetErrno(PSX_ERANGE);
            return NULL;
        }

        Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, AllocSize);
        if (Buffer == NULL)
        {
            PsxSetErrno(PSX_ENOMEM);
            return NULL;
        }
    }
    else if (Size < Needed)
    {
        PsxSetErrno(PSX_ERANGE);
        return NULL;
    }

    RtlMoveMemory(Buffer, Temp, Needed);
    return Buffer;
}

/**
 * @brief Changes the current directory. The new NT path keeps a trailing separator.
 */
int
__cdecl
chdir(
    _In_z_ const char *Path)
{
    CHAR NtPath[PSX_PATH_MAX];
    ULONG Length;

    if (access(Path, PSX_F_OK) != 0)
        return -1;

    Length = PsxBuildNtPath(Path, NtPath, sizeof(NtPath));
    if (Length == 0 || Length + 2 >= sizeof(PsxStartupCwd))
    {
        PsxSetErrno(PSX_ENOENT);
        return -1;
    }

    RtlCopyMemory(PsxStartupCwd, NtPath, Length);
    if (PsxStartupCwd[Length - 1] != '\\')
        PsxStartupCwd[Length++] = '\\';
    PsxStartupCwd[Length] = '\0';
    PsxStartupCwdLen = Length;
    return 0;
}

/**
 * @brief Like isatty, but asks the session leader to confirm the terminal.
 */
int
__cdecl
isatty2(
    _In_ int FileDescriptor)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiIsatty, PSX_BODY_DATALEN(2 * sizeof(ULONG)));
    ((PULONG)Message.Data.Raw)[0] = (ULONG)FileDescriptor;
    ((PULONG)Message.Data.Raw)[1] = 1;
    if (PsxCallServer(&Message) < 0)
        return 0;

    return (int)((PULONG)Message.Data.Raw)[1];
}

/**
 * @brief Looks up Name in the environment published by __PdxInitializeData.
 */
char *
__cdecl
getenv(
    _In_opt_z_ const char *Name)
{
    char **Env;
    char *Entry;
    ULONG NameLength;
    ULONG i;

    if (Name == NULL || PsxEnvironLocation == NULL || *PsxEnvironLocation == NULL)
        return NULL;

    NameLength = PsxStringLengthA(Name);

    for (Env = *PsxEnvironLocation; *Env != NULL; Env++)
    {
        Entry = *Env;
        for (i = 0; i < NameLength; i++)
        {
            if (Entry[i] != Name[i])
                break;
        }

        if (i == NameLength && Entry[NameLength] == '=')
            return Entry + NameLength + 1;
    }

    return NULL;
}

/**
 * @brief Returns the packed argv/environment table the spawner stored as the
 * process command line.
 */
char *
__cdecl
__PdxGetCmdLine(void)
{
    PPEB Peb = NtCurrentPeb();

    if (Peb != NULL && Peb->ProcessParameters != NULL)
        return (char *)Peb->ProcessParameters->CommandLine.Buffer;

    return NULL;
}

/**
 * @brief Copies Source into Destination, or into a static buffer when
 * Destination is NULL. At most 15 characters are copied.
 */
static
char *
PsxCopyOrStatic(
    _Out_writes_opt_z_(16) char *Destination,
    _In_z_ const char *Source)
{
    static char StaticBuffer[16];
    char *Out = (Destination != NULL) ? Destination : StaticBuffer;
    int i = 0;

    while (Source[i] != '\0' && i < 15)
    {
        Out[i] = Source[i];
        i++;
    }
    Out[i] = '\0';

    return Out;
}

/* There is a single controlling terminal and a fixed login name */
char *
__cdecl
ctermid(
    _Out_writes_opt_z_(16) char *Buffer)
{
    return PsxCopyOrStatic(Buffer, "/dev/tty");
}

char *
__cdecl
cuserid(
    _Out_writes_opt_z_(16) char *Buffer)
{
    return PsxCopyOrStatic(Buffer, "root");
}

char *
__cdecl
getlogin(void)
{
    static char Name[] = "root";

    return Name;
}

char *
__cdecl
ttyname(
    _In_ int FileDescriptor)
{
    static char Name[] = "/dev/tty";

    return isatty(FileDescriptor) ? Name : NULL;
}

/* The identity comes from the NT token and cannot be changed, only reaffirmed */
int
__cdecl
setuid(
    _In_ unsigned int Uid)
{
    if (Uid == (unsigned int)getuid())
        return 0;

    PsxSetErrno(PSX_EPERM);
    return -1;
}

int
__cdecl
setgid(
    _In_ unsigned int Gid)
{
    if (Gid == (unsigned int)getgid())
        return 0;

    PsxSetErrno(PSX_EPERM);
    return -1;
}

/**
 * @brief Returns the descriptor stored in the _file member of a FILE.
 */
int
__cdecl
fileno(
    _In_opt_ void *Stream)
{
    if (Stream == NULL)
    {
        PsxSetErrno(PSX_EBADF);
        return -1;
    }

    return (int)*((signed char *)Stream + PSX_IOBUF_FILE_OFFSET);
}

int
__cdecl
getreg(
    _In_ int Which)
{
    UNREFERENCED_PARAMETER(Which);
    PsxSetErrno(PSX_ENOSYS);
    return -1;
}
