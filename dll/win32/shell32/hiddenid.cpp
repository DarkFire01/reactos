/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Data blocks hidden at the end of an item ID
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

/* Header plus the string type WORD that follows it */
#define HIDDENSTRING_HEADER     (sizeof(IDHIDDENBLOCK) + sizeof(WORD))

static
BOOL
IsHiddenBlockAt(
    _In_ const BYTE *pBlock,
    _In_ const BYTE *pEnd)
{
    PCIDHIDDENBLOCK pHeader = (PCIDHIDDENBLOCK)pBlock;

    if (pBlock + sizeof(*pHeader) > pEnd)
        return FALSE;

    return pHeader->cb >= sizeof(*pHeader) && HIDDENID_IS_VALID(pHeader->dwId) &&
           pBlock + pHeader->cb <= pEnd;
}

PIDHIDDENBLOCK
WINAPI
ILFindFirstHiddenID(
    _In_ LPCITEMIDLIST pidlItem)
{
    const BYTE *pItem = (const BYTE *)pidlItem;
    const BYTE *pEnd = pItem + pidlItem->mkid.cb;
    WORD cbOffset;

    if (pidlItem->mkid.cb < sizeof(WORD))
        return NULL;

    /* The trailing WORD points at the first block; zero means the item has none */
    cbOffset = *(const WORD *)(pEnd - sizeof(WORD));
    if (!cbOffset || (SIZE_T)cbOffset + sizeof(IDHIDDENBLOCK) >= pidlItem->mkid.cb)
        return NULL;

    if (!IsHiddenBlockAt(pItem + cbOffset, pEnd))
        return NULL;

    return (PIDHIDDENBLOCK)(pItem + cbOffset);
}

PIDHIDDENBLOCK
WINAPI
ILNextHidden(
    _In_ PCIDHIDDENBLOCK pBlock,
    _In_ const BYTE *pEnd)
{
    const BYTE *pNext = (const BYTE *)pBlock + pBlock->cb;

    if ((const BYTE *)pBlock + sizeof(*pBlock) > pEnd || pNext >= pEnd)
        return NULL;

    /* A block that does not move forward would loop forever */
    if (pNext < (const BYTE *)(pBlock + 1))
        return NULL;

    return IsHiddenBlockAt(pNext, pEnd) ? (PIDHIDDENBLOCK)pNext : NULL;
}

PIDHIDDENBLOCK
WINAPI
ILFindHiddenIDOn(
    _In_opt_ LPCITEMIDLIST pidl,
    _In_ DWORD dwId,
    _In_ BOOL fLastItem)
{
    LPCITEMIDLIST pidlItem = pidl;
    PIDHIDDENBLOCK pBlock;
    const BYTE *pEnd;

    if (!pidl || !pidl->mkid.cb)
        return NULL;

    if (fLastItem)
        pidlItem = ILFindLastID(pidl);

    pEnd = (const BYTE *)pidlItem + pidlItem->mkid.cb;
    for (pBlock = ILFindFirstHiddenID(pidlItem); pBlock; pBlock = ILNextHidden(pBlock, pEnd))
    {
        if (pBlock->dwId == dwId)
            return pBlock;
    }
    return NULL;
}

HRESULT
WINAPI
ILCloneWithHiddenID(
    _In_opt_ LPCITEMIDLIST pidl,
    _In_ PCIDHIDDENBLOCK pBlock,
    _Outptr_ LPITEMIDLIST *ppidl)
{
    LPITEMIDLIST pidlLast;
    PIDHIDDENBLOCK pFirst;
    BYTE *pNewBlock;
    WORD cbFirstOffset;
    WORD cbBlock;
    UINT cbPidl;

    *ppidl = NULL;
    if (!pidl || !pidl->mkid.cb || !HIDDENID_IS_VALID(pBlock->dwId))
        return E_INVALIDARG;

    cbPidl = ILGetSize(pidl);
    *ppidl = (LPITEMIDLIST)CoTaskMemAlloc(cbPidl + pBlock->cb + sizeof(WORD));
    if (!*ppidl)
        return E_OUTOFMEMORY;

    CopyMemory(*ppidl, pidl, cbPidl);
    pidlLast = ILFindLastID(*ppidl);

    /* The chain keeps its start; an item without one starts it with this block */
    pFirst = ILFindFirstHiddenID(pidlLast);
    if (pFirst)
        cbFirstOffset = *(WORD *)((BYTE *)pidlLast + pidlLast->mkid.cb - sizeof(WORD));
    else
        cbFirstOffset = pidlLast->mkid.cb;

    /* The block replaces the list terminator and takes over the item's trailing offset WORD */
    pNewBlock = (BYTE *)*ppidl + cbPidl - sizeof(WORD);
    MoveMemory(pNewBlock, pBlock, pBlock->cb);
    cbBlock = pBlock->cb + sizeof(WORD);
    ((PIDHIDDENBLOCK)pNewBlock)->cb = cbBlock;
    pidlLast->mkid.cb += cbBlock;
    *(WORD *)(pNewBlock + cbBlock - sizeof(WORD)) = cbFirstOffset;

    /* New list terminator */
    *(WORD *)(pNewBlock + cbBlock) = 0;
    return S_OK;
}

LPITEMIDLIST
WINAPI
ILAppendHiddenID(
    _In_opt_ LPITEMIDLIST pidl,
    _In_ PCIDHIDDENBLOCK pBlock)
{
    LPITEMIDLIST pidlNew;

    if (!pidl || !pidl->mkid.cb)
        return pidl;

    /* The caller's ID list is replaced, so a failure loses it like a failed realloc */
    ILCloneWithHiddenID(pidl, pBlock, &pidlNew);
    ILFree(pidl);
    return pidlNew;
}

LPITEMIDLIST
WINAPI
ILAppendHiddenStringW(
    _In_opt_ LPITEMIDLIST pidl,
    _In_ DWORD dwId,
    _In_ LPCWSTR pszValue)
{
    LPITEMIDLIST pidlNew = NULL;
    PIDHIDDENBLOCK pBlock;
    SIZE_T cchValue = wcslen(pszValue) + 1;
    SIZE_T cbBlock = HIDDENSTRING_HEADER + cchValue * sizeof(WCHAR);

    if (cbBlock > MAXWORD)
        return NULL;

    pBlock = (PIDHIDDENBLOCK)LocalAlloc(LPTR, cbBlock);
    if (!pBlock)
        return NULL;

    pBlock->cb = (WORD)cbBlock;
    pBlock->dwId = dwId;
    *(WORD *)(pBlock + 1) = HIDDENSTRING_UNICODE;
    if (SUCCEEDED(StringCchCopyW((LPWSTR)((BYTE *)pBlock + HIDDENSTRING_HEADER), cchValue, pszValue)))
        pidlNew = ILAppendHiddenID(pidl, pBlock);

    LocalFree(pBlock);
    return pidlNew;
}

BOOL
WINAPI
ILGetHiddenStringW(
    _In_opt_ LPCITEMIDLIST pidl,
    _In_ DWORD dwId,
    _Out_writes_(cchValue) LPWSTR pszValue,
    _In_ UINT cchValue)
{
    PIDHIDDENBLOCK pBlock = ILFindHiddenIDOn(pidl, dwId, TRUE);
    const BYTE *pString;

    if (!pBlock)
        return FALSE;

    pString = (const BYTE *)pBlock + HIDDENSTRING_HEADER;
    if (*(const WORD *)(pBlock + 1) == HIDDENSTRING_UNICODE)
        lstrcpynW(pszValue, (LPCWSTR)pString, cchValue);
    else
        SHAnsiToUnicode((LPCSTR)pString, pszValue, cchValue);
    return TRUE;
}

BOOL
WINAPI
ILGetHiddenStringAllocW(
    _In_opt_ LPCITEMIDLIST pidl,
    _In_ DWORD dwId,
    _Outptr_ LPWSTR *ppszValue)
{
    PIDHIDDENBLOCK pBlock = ILFindHiddenIDOn(pidl, dwId, TRUE);
    LPCWSTR pszString;
    SIZE_T cbString;

    *ppszValue = NULL;
    if (!pBlock || *(const WORD *)(pBlock + 1) != HIDDENSTRING_UNICODE)
        return FALSE;

    /* The block sits at an odd place inside the item, so the string may be unaligned */
    pszString = (LPCWSTR)((const BYTE *)pBlock + HIDDENSTRING_HEADER);
    cbString = (wcslen(pszString) + 1) * sizeof(WCHAR);
    *ppszValue = (LPWSTR)CoTaskMemAlloc(cbString);
    if (!*ppszValue)
        return FALSE;

    CopyMemory(*ppszValue, pszString, cbString);
    return TRUE;
}

BOOL
WINAPI
ILRemoveHiddenID(
    _In_opt_ LPCITEMIDLIST pidl,
    _In_ DWORD dwId)
{
    PIDHIDDENBLOCK pBlock = ILFindHiddenIDOn(pidl, dwId, TRUE);

    if (!pBlock)
        return FALSE;

    pBlock->dwId = HIDDENID_REMOVED;
    return TRUE;
}
