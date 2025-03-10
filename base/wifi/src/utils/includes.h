/*
 * wpa_supplicant/hostapd - Default include files
 * Copyright (c) 2005-2006, Jouni Malinen <j@w1.fi>
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 *
 * This header file is included into all C files so that commonly used header
 * files can be selected with OS specific ifdef blocks in one place instead of
 * having to have OS/C library specific selection in many files.
 */

#ifndef INCLUDES_H
#define INCLUDES_H

/* Include possible build time configuration before including anything else */
#include "build_config.h"

#include <stdlib.h>
#include <stddef.h>
#if defined(_MSC_VER) && (_MSC_VER < 1800)
#ifndef __cplusplus
typedef int bool;
#define true 1
#define false 0
#endif
#else
#include <stdbool.h>
#endif
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#ifndef _WIN32_WCE
#include <signal.h>
#include <sys/types.h>
#include <errno.h>
#endif /* _WIN32_WCE */
#include <ctype.h>

#ifndef _MSC_VER
#include <unistd.h>
#endif /* _MSC_VER */

#ifndef CONFIG_NATIVE_WINDOWS
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifndef __vxworks
#include <sys/uio.h>
#include <sys/time.h>
#endif /* __vxworks */
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#ifndef __MINGW32_VERSION
#include <wspiapi.h>
#endif
#include <windows.h>

#include "common.h"

#if defined(_MSC_VER) && (_MSC_VER < 1800)
static int isblank_impl(int c) { return (c==' ' || c=='\t'); }
#define isblank isblank_impl
#endif

#if defined(_MSC_VER) && !defined(STDIN_FILENO)
#define STDIN_FILENO _fileno(stdin)
#endif

#include <errno.h>
#ifndef EOPNOTSUPP
#define EOPNOTSUPP ENOSYS
#endif
#ifndef EAFNOSUPPORT
#define EAFNOSUPPORT ENOSYS
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif

#include <assert.h>
#include "inet_pton.h"
#include "inet_ntop.h"
#define inet_pton inet_pton_impl
#define inet_ntop inet_ntop_impl

#if defined(_MSC_VER) && !defined(S_IRUSR)
#define S_IRUSR _S_IREAD
#endif
#if defined(_MSC_VER) && !defined(S_IWUSR)
#define S_IWUSR _S_IWRITE
#endif
#if defined(_WIN32) && !defined(S_IRGRP)
#define S_IRGRP _S_IREAD
#endif

#endif /* CONFIG_NATIVE_WINDOWS */

#endif /* INCLUDES_H */
