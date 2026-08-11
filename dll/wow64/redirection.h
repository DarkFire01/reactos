/*
 * Wow64 filesystem and registry redirection
 *
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         wow64.dll
 * FILE:            dll/wow64/redirection.h
 * PROGRAMMER:      Marcin Jabłoński
 */

#pragma once

/* FIXME: for now, the WOW64 directory path is hardcoded. */
#define TMP_WOW_DIR L"C:\\ReactOS\\SysWOW64"

typedef struct _WOW64_PATH_REDIRECTION
{
    UNICODE_STRING From;
    UNICODE_STRING To;
} WOW64_PATH_REDIRECTION, *PWOW64_PATH_REDIRECTION;
 
BOOLEAN 
RedirectPath(const WOW64_PATH_REDIRECTION* Redirection, 
             POBJECT_ATTRIBUTES ObjectAttributes);
 
BOOLEAN 
GetFileRedirect(OBJECT_ATTRIBUTES* attr);