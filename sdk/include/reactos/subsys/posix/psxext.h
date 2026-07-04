/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     POSIX subsystem *extension* opcodes -- operations beyond the faithful
 *              psxss dispatch table (ApiNumber 0x00..0x3E, PsxApiMaxApiNumber 0x3F).
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifndef _PSXEXT_H_
#define _PSXEXT_H_

//
// poll a single descriptor for readability (the primitive real libX11's event core
// needs: it select()s the display fd). Reuses the read/write body slots to avoid a
// new arg struct: FileDescriptor = the fd, Count = timeout in milliseconds
// (0 = non-blocking poll, 0xFFFFFFFF = infinite). ReturnValue: 1 = readable now,
// 0 = timed out, -1 = error (Errno set). Multi-fd select() is a later extension.
//
#define PSX_API_POLL    0x40

//
// The same single-fd readability poll, tunneled through fcntl(fd, cmd, arg) --
// ApiNumber 0x2D, which the REAL MS psxdll.dll marshals generically (cmd in the
// message body, no client-side validation).  This is how clients built against
// the unmodified 1996 psxdll reach the poll primitive: no new export needed.
//   fcntl(fd, PSX_FCNTL_POLLRD, timeout_ms)  ->  1 readable, 0 timed out, -1 error
//   (timeout_ms 0 = non-blocking probe, -1/0xFFFFFFFF = infinite)
// The NT4 ABI uses F_* commands 0..7; 0x70 is safely outside that range.
//
#define PSX_FCNTL_POLLRD    0x70

//
// ioctl(fd, request, arg) -- ApiNumber 0x41. Body: FileDescriptor = fd (0x30),
// Raw[1] = request (a TIOC* code), Raw[2] = arg (a client pointer, request-
// specific). Used by the pseudo-terminal layer: TIOCGPTN (get pts index),
// TIOCSWINSZ/TIOCGWINSZ (window size), TIOCSCTTY (make controlling tty; no-op).
//
#define PSX_API_IOCTL   0x41

//
// select() -- ApiNumber 0x42. Multi-fd readability wait built on the same ~10ms
// poll loop as PSX_API_POLL. Body: Raw[0] = read-fd count N (<= 32), Raw[1] =
// timeout ms (0 = poll, 0xFFFFFFFF = infinite), Raw[2..2+N-1] = the read fds.
// ReturnValue = a bitmask (bit i set => read fd i is readable), 0 on timeout,
// -1 on error. Writability is treated as always-ready by the client.
//
#define PSX_API_SELECT  0x42
#define PSX_SELECT_MAXFDS   32

#endif /* _PSXEXT_H_ */
