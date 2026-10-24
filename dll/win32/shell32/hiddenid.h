/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Data blocks hidden at the end of an item ID
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/*
 * An item ID can end in a chain of extra blocks the folder that made it never parses. The last WORD
 * of the item is the offset of the first block, every block starts with this header and its ID
 * carries 0xBEEF in the high word.
 */
#include <pshpack1.h>
typedef struct _IDHIDDENBLOCK
{
    WORD cb;
    WORD wVersion;
    DWORD dwId;
} IDHIDDENBLOCK, *PIDHIDDENBLOCK;
typedef const IDHIDDENBLOCK *PCIDHIDDENBLOCK;
#include <poppack.h>

#define HIDDENID_SIGNATURE      0xBEEF
#define HIDDENID_IS_VALID(id)   (HIWORD(id) == HIDDENID_SIGNATURE)

/* An ID removed from the chain keeps its place but no longer matches anything */
#define HIDDENID_REMOVED        0xBEEF0000

/* The file system item data every file and folder ID carries */
#define HIDDENID_FILESYSTEM     0xBEEF0004

/* AppUserModelID of a pinned item, stored as a string block */
#define HIDDENID_APPID          0xBEEF001D

/* String block payload types after the header */
#define HIDDENSTRING_ANSI       1
#define HIDDENSTRING_UNICODE    2

PIDHIDDENBLOCK
WINAPI
ILFindFirstHiddenID(
    _In_ LPCITEMIDLIST pidlItem);

PIDHIDDENBLOCK
WINAPI
ILNextHidden(
    _In_ PCIDHIDDENBLOCK pBlock,
    _In_ const BYTE *pEnd);

PIDHIDDENBLOCK
WINAPI
ILFindHiddenIDOn(
    _In_opt_ LPCITEMIDLIST pidl,
    _In_ DWORD dwId,
    _In_ BOOL fLastItem);

HRESULT
WINAPI
ILCloneWithHiddenID(
    _In_opt_ LPCITEMIDLIST pidl,
    _In_ PCIDHIDDENBLOCK pBlock,
    _Outptr_ LPITEMIDLIST *ppidl);

LPITEMIDLIST
WINAPI
ILAppendHiddenID(
    _In_opt_ LPITEMIDLIST pidl,
    _In_ PCIDHIDDENBLOCK pBlock);

LPITEMIDLIST
WINAPI
ILAppendHiddenStringW(
    _In_opt_ LPITEMIDLIST pidl,
    _In_ DWORD dwId,
    _In_ LPCWSTR pszValue);

BOOL
WINAPI
ILGetHiddenStringW(
    _In_opt_ LPCITEMIDLIST pidl,
    _In_ DWORD dwId,
    _Out_writes_(cchValue) LPWSTR pszValue,
    _In_ UINT cchValue);

BOOL
WINAPI
ILGetHiddenStringAllocW(
    _In_opt_ LPCITEMIDLIST pidl,
    _In_ DWORD dwId,
    _Outptr_ LPWSTR *ppszValue);

BOOL
WINAPI
ILRemoveHiddenID(
    _In_opt_ LPCITEMIDLIST pidl,
    _In_ DWORD dwId);
