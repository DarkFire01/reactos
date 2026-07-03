/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     POSIX subsystem extension opcodes beyond the base dispatch table
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifndef _PSXEXT_H_
#define _PSXEXT_H_

//
// Single descriptor readability poll. Reuses the read/write body: FileDescriptor is the fd,
// Count is the timeout in ms (0 polls, 0xFFFFFFFF waits forever).
// ReturnValue is 1 when readable, 0 on timeout, -1 on error.
//
#define PSX_API_POLL    0x40

//
// The same poll tunneled through fcntl(fd, PSX_FCNTL_POLLRD, timeout_ms), so clients
// without a poll export can reach it. Chosen outside the F_* command range (0..7).
//
#define PSX_FCNTL_POLLRD    0x70

//
// ioctl(fd, request, arg). Body: FileDescriptor (0x30), Raw[1] = TIOC* request,
// Raw[2] = client pointer argument.
//
#define PSX_API_IOCTL   0x41

//
// Multi-fd readability wait. Body: Raw[0] = read fd count N (up to PSX_SELECT_MAXFDS),
// Raw[1] = timeout in ms (same encoding as PSX_API_POLL), Raw[2..2+N-1] = the read fds.
// ReturnValue is a bitmask of readable fds, 0 on timeout, -1 on error.
//
#define PSX_API_SELECT  0x42
#define PSX_SELECT_MAXFDS   32

#endif /* _PSXEXT_H_ */
