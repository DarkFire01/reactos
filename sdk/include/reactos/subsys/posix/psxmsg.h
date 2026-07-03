/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     LPC protocol shared by the POSIX client (PSXDLL.DLL) and the
 *              POSIX server (PSXSS.EXE)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

//
// This header describes the on-the-wire LPC contract between PSXDLL (client)
// and PSXSS (server). The message layout mirrors the Microsoft NT 4.0
// implementation recovered under /decompanalysis, so that our reimplementation
// is ABI-compatible with the original binaries during bring-up.
//
// Requires the NDK definitions (PORT_MESSAGE, UNICODE_STRING, FIELD_OFFSET,
// C_ASSERT) to be included beforehand, e.g. <ndk/lpctypes.h>.
//

#define PSX_PROTOCOL_VERSION    1


/* OBJECT NAMESPACE **********************************************************/

//
// All POSIX subsystem objects live under the \PSXSS object directory. The
// server publishes a global control port and, per session, a session port plus
// a shared data section. Per-session names are formatted with the session id.
//
#define PSX_SS_OBJECT_DIRECTORY     L"\\PSXSS"
#define PSX_API_PORT_NAME           L"\\PSXSS\\ApiPort"     // global control port
#define PSX_SB_PORT_NAME            L"\\PSXSS\\SESPORT"     // session-register port

#define PSX_SESSION_PORT_TEMPLATE   L"\\PSXSS\\PSXSES\\P%u" // per-session LPC port
#define PSX_SESSION_DATA_TEMPLATE   L"\\PSXSS\\PSXSES\\D%u" // per-session data section


/* SHARED DATA SECTION *******************************************************/

//
// Bulk payloads (read/write buffers, path strings, struct stat, exec argv) do
// not travel inside the LPC message. They are written into a section mapped by
// both the client and the server; only a server-relative pointer is sent (the
// client adds the server/client base delta before sending).
//
#define PSX_SESSION_SECTION_SIZE    0x8000      // 32 KiB per-session data section
                                                // (the NT 4.0 ABI size; psxss accepts
                                                // client views up to 0x8000)
#define PSX_SESSION_SECTION_OFFSET  0x2000      // first 8 KiB reserved

//
// Bulk-transfer sub-ops carried in the DataChannel field of the session-port
// data message (the second phase of read/write). The byte payload moves through
// the shared section, up to one page (4096 bytes) at a time.
//
#define PSX_DATA_OP_READ    3
#define PSX_DATA_OP_WRITE   4


/* API NUMBERS ***************************************************************/

//
// PSXSS dispatches an incoming request through a 63-entry function-pointer
// table, indexed by the ApiNumber field of the message body. The values below
// are the original psx opcodes. Honestly we can add to this as much as we wish.
//
typedef enum _PSX_API_NUMBER
{
    PsxApiFork          = 0x00,
    PsxApiExecve        = 0x01,
    PsxApiWaitpid       = 0x02,
    PsxApiExit          = 0x03,
    PsxApiKill          = 0x04,
    PsxApiSigaction     = 0x05,
    PsxApiSigprocmask   = 0x06,
    PsxApiSigpending    = 0x07,
    PsxApiSigsuspend    = 0x08, // (pause reuses this opcode)
    PsxApiAlarm         = 0x09, // alarm/setitimer (per-process timer)
    PsxApiGetIds        = 0x0A, // identity bundle (getpid/getuid/getgid/...)
    PsxApiGetGroups     = 0x0D,
    PsxApiSetsid        = 0x10,
    PsxApiSetpgid       = 0x11,
    PsxApiTimes         = 0x14,
    PsxApiIsatty        = 0x16, // isatty/isatty2 (+ data-channel sub-op 6/7)
    PsxApiSysconf       = 0x17,
    PsxApiOpen          = 0x18,
    PsxApiUmask         = 0x19,
    PsxApiLink          = 0x1A,
    PsxApiMkdir         = 0x1B,
    PsxApiMkfifo        = 0x1C, // mkfifo/mknod (regular node tagged FILE_ATTR_SYSTEM)
    PsxApiUnlink        = 0x1D,
    PsxApiRename        = 0x1E,
    PsxApiStat          = 0x1F,
    PsxApiFstat         = 0x20,
    PsxApiAccess        = 0x21,
    PsxApiChmod         = 0x22,
    PsxApiChown         = 0x23,
    PsxApiUtime         = 0x24,
    PsxApiPathconf      = 0x25,
    PsxApiFpathconf     = 0x26,
    PsxApiPipe          = 0x27,
    PsxApiDup           = 0x28,
    PsxApiDup2          = 0x29,
    PsxApiClose         = 0x2A,
    PsxApiRead          = 0x2B, // control message; bulk via PSX_DATA_OP_READ
    PsxApiWrite         = 0x2C, // control message; bulk via PSX_DATA_OP_WRITE
    PsxApiFcntl         = 0x2D, // fcntl (cmd at body +0x40)
    PsxApiLseek         = 0x2E,
    PsxApiTcgetattr     = 0x2F, // tty control family 0x2F-0x36 (psxss defers to posix.exe)
    PsxApiTcsetattr     = 0x30,
    PsxApiGetpwuid      = 0x37, // getpwuid (getlogin reuses this)
    PsxApiGetpwnam      = 0x38,
    PsxApiGetgrgid      = 0x39,
    PsxApiGetgrnam      = 0x3A,
    PsxApiRmdir         = 0x3B,
    PsxApiReaddir       = 0x3C, // one dir entry per call; name poked into DIR buffer
    PsxApiFtruncate     = 0x3D,
    PsxApiClearExecFlag = 0x3E, // post-execve ack: clears the exec-in-progress flag

    PsxApiMaxApiNumber  = 0x3F  // the dispatch table has 0x3F (63) slots
} PSX_API_NUMBER;


/* CONNECTION ****************************************************************/

//
// A client states the kind of connection it opens (via the NtConnectPort
// connection data blob) so the server can route it to the right handler.
//
typedef enum _PSX_CONNECTION_TYPE
{
    PsxConnectionProcess,   // a POSIX process connecting to the API port
    PsxConnectionTerminal,  // POSIXTERM.EXE registering a controlling terminal
    PsxConnectionServer     // subsystem-to-subsystem (SB) registration
} PSX_CONNECTION_TYPE;

typedef struct _PSX_CONNECT_INFO
{
    PSX_CONNECTION_TYPE ConnectionType; // IN
    ULONG               Version;        // IN/OUT: PSX_PROTOCOL_VERSION
    ULONG               SessionId;      // OUT: assigned by the server
} PSX_CONNECT_INFO, *PPSX_CONNECT_INFO;


/* PER-API ARGUMENTS *********************************************************/

//
// The structures below overlay the message Data area (message offset 0x30).
// Pointer fields (Path.Buffer, StatBuffer) are server-relative addresses into
// the shared section.
//

// PsxApiOpen
typedef struct _PSX_OPEN_REQUEST
{
    UNICODE_STRING Path;        // 0x30: path in the shared section (server view)
    ULONG          OpenFlag;    // 0x38: POSIX O_* flags
    ULONG          Mode;        // 0x3C: creation mode (used when O_CREAT)
} PSX_OPEN_REQUEST;

// PsxApiRead / PsxApiWrite (control phase)
typedef struct _PSX_RW_REQUEST
{
    ULONG FileDescriptor;       // 0x30
    ULONG Buffer;               // 0x34: caller buffer (informational)
    ULONG Count;                // 0x38: requested byte count
    ULONG Reserved[2];          // 0x3C
    ULONG HasData;              // 0x44: server sets when a data phase follows
} PSX_RW_REQUEST;

// PsxApiLseek (Offset is reused in the reply for the resulting position)
typedef struct _PSX_LSEEK_REQUEST
{
    ULONG FileDescriptor;       // 0x30
    ULONG Whence;               // 0x34: SEEK_SET / SEEK_CUR / SEEK_END
    LONG  Offset;               // 0x38: in: offset, out: new file position
} PSX_LSEEK_REQUEST;

// PsxApiStat
typedef struct _PSX_STAT_REQUEST
{
    UNICODE_STRING Path;        // 0x30: path in the shared section (server view)
    ULONG          StatBuffer;  // 0x38: server-relative ptr to a 40-byte stat
} PSX_STAT_REQUEST;

// PsxApiFstat
typedef struct _PSX_FSTAT_REQUEST
{
    ULONG FileDescriptor;       // 0x30
    ULONG StatBuffer;           // 0x34: server-relative ptr to a 40-byte stat
} PSX_FSTAT_REQUEST;

// PsxApiGetIds (reply only; these POSIX calls cannot fail)
typedef struct _PSX_IDS_REPLY
{
    ULONG Pid;                  // 0x30
    ULONG ParentPid;            // 0x34
    ULONG ProcessGroup;         // 0x38
    ULONG Uid;                  // 0x3C
    ULONG EffectiveUid;         // 0x40
    ULONG Gid;                  // 0x44
    ULONG EffectiveGid;         // 0x48
} PSX_IDS_REPLY;


/* API MESSAGE ***************************************************************/

//
// Size of the inline argument/data area that follows the fixed reply fields.
//
#define PSX_MAX_API_DATA    64

//
// Retry tag the server places in a reply when a call was interrupted by a
// signal; the client resends the request when it sees this with Errno == EINTR.
//
#define PSX_RETRY_TAG       15

//
// The POSIX API message. PORT_MESSAGE is the standard 24-byte NT LPC header;
// the POSIX body begins immediately after it. The server reads ApiNumber at
// body offset 0x04 (message offset 0x1C) -- the field offsets are asserted
// for vaguely keeping track against the og psxss.
//
typedef struct _PSX_API_MESSAGE
{
    PORT_MESSAGE Header;        // 0x00: NT LPC header

    ULONG        DataChannel;   // 0x18: bulk-data sub-op (PSX_DATA_OP_*) / reserved
    ULONG        ApiNumber;     // 0x1C: requested operation (PSX_API_NUMBER)
    LONG         Errno;         // 0x20: POSIX errno, 0 on success (reply)
    LONG         ReturnValue;   // 0x24: POSIX return value (reply)
    NTSTATUS     Status;        // 0x28: secondary NT status (reply)
    ULONG        RetryTag;      // 0x2C: PSX_RETRY_TAG when EINTR -> resend

    union                       // 0x30: per-call arguments / inline data
    {
        UCHAR             Raw[PSX_MAX_API_DATA];
        PSX_OPEN_REQUEST  Open;
        PSX_RW_REQUEST    ReadWrite;
        PSX_LSEEK_REQUEST Lseek;
        PSX_STAT_REQUEST  Stat;
        PSX_FSTAT_REQUEST Fstat;
        PSX_IDS_REPLY     Ids;
    } Data;
} PSX_API_MESSAGE, *PPSX_API_MESSAGE;

#if defined(_M_IX86)
// Fixed reply fields (locked to the NT 4.0 body layout).
C_ASSERT(FIELD_OFFSET(PSX_API_MESSAGE, ApiNumber) == 0x1C);
C_ASSERT(FIELD_OFFSET(PSX_API_MESSAGE, Errno) == 0x20);
C_ASSERT(FIELD_OFFSET(PSX_API_MESSAGE, ReturnValue) == 0x24);
C_ASSERT(FIELD_OFFSET(PSX_API_MESSAGE, Data) == 0x30);
// Per-API argument offsets.
C_ASSERT(FIELD_OFFSET(PSX_API_MESSAGE, Data.Open.OpenFlag) == 0x38);
C_ASSERT(FIELD_OFFSET(PSX_API_MESSAGE, Data.ReadWrite.HasData) == 0x44);
C_ASSERT(FIELD_OFFSET(PSX_API_MESSAGE, Data.Stat.StatBuffer) == 0x38);
C_ASSERT(FIELD_OFFSET(PSX_API_MESSAGE, Data.Ids.EffectiveGid) == 0x48);
#endif
