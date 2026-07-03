/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Controlling terminal LPC protocol served by POSIX.EXE
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

//
// The controlling terminal lives in POSIX.EXE, not PSXSS. Requests arrive on the session
// port \PSXSS\PSXSES\P<id>; tty data moves through the section \PSXSS\PSXSES\D<id>.
//

#include "psxmsg.h"     // PSX_SESSION_PORT_TEMPLATE, PSX_SESSION_DATA_TEMPLATE
#include <termios.h>    // struct termios and constants

#if defined(_M_IX86)
C_ASSERT(sizeof(struct termios) == 0x44);
#endif


/* SESSION / TERMINAL LPC PROTOCOL *******************************************/

//
// Per-session data section backing tty reads and writes. The first 8 KiB are reserved.
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
// Sub-operation for PsxTermApiIo (first argument word of the request)
//
typedef enum _PSX_TERMINAL_IO_OP
{
    PsxTermIoOpenTty   = 1,  // open the controlling tty (CONIN$/CONOUT$)
    PsxTermIoClose     = 2,  // close (no-op)
    PsxTermIoRead      = 3,  // read(fd, buf, len) into the shared section
    PsxTermIoWrite     = 4,  // write(fd, buf, len) from the shared section
    PsxTermIoReadChar  = 5,  // read a single console key
    PsxTermIoIsatty    = 6,  // isatty(fd)
    PsxTermIoIsatty2   = 7   // isatty(fd), alternate opcode
} PSX_TERMINAL_IO_OP;

//
// Direction for PsxTermApiTcAttr (first argument word of the request)
//
typedef enum _PSX_TERMINAL_TCATTR_DIR
{
    PsxTermTcGetAttr = 0,  // copy the server termios out to the client
    PsxTermTcSetAttr = 1   // take the client termios and apply the console mode
} PSX_TERMINAL_TCATTR_DIR;
