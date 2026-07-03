/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     The controlling-terminal LPC protocol served by POSIX.EXE on its
 *              per-session port.
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

//
// The terminal lives in POSIX.EXE (the per-session controlling terminal /
// VT emulator over the Win32 console), NOT in PSXSS. A POSIX process talks to
// it over the session port \PSXSS\PSXSES\P<id>, and bulk tty bytes move through
// the per-session data section \PSXSS\PSXSES\D<id>.
//

#include "psxmsg.h"     // PSX_SESSION_PORT_TEMPLATE, PSX_SESSION_DATA_TEMPLATE
#include <termios.h>    // struct termios + constants (POSIX SDK, 1:1 with MSTOOLS)

#if defined(_M_IX86)
// OG PSXSS assert for compat.
C_ASSERT(sizeof(struct termios) == 0x44);
#endif


/* SESSION / TERMINAL LPC PROTOCOL *******************************************/

//
// The per-session data section that backs tty reads/writes (the \D<id> object).
// 64 KiB, the first 8 KiB reserved (matches PSX_SESSION_SECTION_* in psxmsg.h).
//
#define PSX_TERMINAL_SECTION_SIZE   0x10000
#define PSX_TERMINAL_SECTION_OFFSET 0x2000

//
// Top-level request id on the session port
//
typedef enum _PSX_TERMINAL_API
{
    PsxTermApiIo      = 0,  // I/O group; the operation is in PSX_TERMINAL_IO_OP
    PsxTermApiExit    = 1,  // process exit / session teardown
    PsxTermApiTcAttr  = 2   // tcgetattr / tcsetattr (see PSX_TERMINAL_TCATTR_DIR)
} PSX_TERMINAL_API;

//
// Sub-operation for PsxTermApiIo (the first argument word of the request).
//
typedef enum _PSX_TERMINAL_IO_OP
{
    PsxTermIoOpenTty   = 1,  // open the controlling tty (CONIN$/CONOUT$)
    PsxTermIoClose     = 2,  // close / no-op
    PsxTermIoRead      = 3,  // read(fd, buf, len) -> shared section
    PsxTermIoWrite     = 4,  // write(fd, buf, len) <- shared section
    PsxTermIoReadChar  = 5,  // read a single console key
    PsxTermIoIsatty    = 6,  // isatty(fd)  (6 and 7 both observed)
    PsxTermIoIsatty2   = 7
} PSX_TERMINAL_IO_OP;

//
// Direction for PsxTermApiTcAttr (the first argument word of the request).
//
typedef enum _PSX_TERMINAL_TCATTR_DIR
{
    PsxTermTcGetAttr = 0,  // copy the server's termios out to the client
    PsxTermTcSetAttr = 1   // take the client's termios and apply console mode
} PSX_TERMINAL_TCATTR_DIR;
