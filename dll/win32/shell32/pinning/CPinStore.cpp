/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Pinned list storage and the shortcut copies kept for pinned items
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"
#include "CPinnedList.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

/* Lets a pinned link point at another link */
#define PIN_SLDF_ALLOW_LINK_TO_LINK     0x00800000

/* Pin stream entries name the folder they sit in by CSIDL so they survive a moved profile */
static const BYTE g_PinFolderCsidls[] =
{
    CSIDL_PROGRAMS,
    CSIDL_STARTMENU,
    CSIDL_MYPICTURES,
    CSIDL_MYMUSIC,
    CSIDL_MYVIDEO,
    CSIDL_PERSONAL,
    CSIDL_COMMON_PROGRAMS,
    CSIDL_COMMON_STARTMENU,
    CSIDL_DESKTOPDIRECTORY,
    CSIDL_COMMON_DESKTOPDIRECTORY,
    CSIDL_PROGRAM_FILES_COMMON,
    CSIDL_PROGRAM_FILES,
    CSIDL_PROGRAM_FILES_COMMONX86,
    CSIDL_PROGRAM_FILESX86,
    CSIDL_APPDATA,
    CSIDL_COMMON_APPDATA,
    CSIDL_SYSTEM,
    CSIDL_SYSTEMX86,
    CSIDL_WINDOWS,
    CSIDL_PROFILE,
};

/* Ends the pin stream */
#define PIN_STREAM_END  0xFF

static
INT
CALLBACK
FreePinItemCallback(
    _In_ PVOID pItem,
    _In_opt_ PVOID pData)
{
    PPIN_ITEM pPin = (PPIN_ITEM)pItem;

    UNREFERENCED_PARAMETER(pData);
    ILFree(pPin->pidl);
    pPin->cbLink = 0;
    if (pPin->psl)
    {
        pPin->psl->Release();
        pPin->psl = NULL;
    }
    return TRUE;
}

static
VOID
ForgetSavedLink(
    _Inout_ PPIN_ITEM pItem)
{
    pItem->cbLink = 0;
    if (pItem->psl)
    {
        pItem->psl->Release();
        pItem->psl = NULL;
    }
}

CPinStore::CPinStore() :
    m_pstmLinks(NULL)
{
    m_hdsa = DSA_Create(sizeof(PIN_ITEM), 4);
}

CPinStore::~CPinStore()
{
    if (m_pstmLinks)
        m_pstmLinks->Release();
    if (m_hdsa)
        DSA_DestroyCallback(m_hdsa, FreePinItemCallback, NULL);
}

int
CPinStore::AppendPidl(
    _In_ PCIDLIST_ABSOLUTE pidl)
{
    PIN_ITEM item = { 0 };
    int iIndex;

    item.pidl = ILClone(pidl);
    if (!item.pidl)
        return -1;

    iIndex = DSA_InsertItem(m_hdsa, DA_LAST, &item);
    if (iIndex < 0)
        ILFree(item.pidl);
    return iIndex;
}

HRESULT
CPinStore::Load(
    _In_ CPinnedListBase *pList)
{
    CComPtr<IEnumFullIDList> penum;
    PIDLIST_ABSOLUTE pidl;
    HRESULT hr;

    hr = pList->EnumObjects(&penum);
    if (FAILED(hr))
        return hr;

    while (penum->Next(1, &pidl, NULL) == S_OK)
    {
        int iIndex = AppendPidl(pidl);
        ILFree(pidl);
        if (iIndex < 0)
            return E_OUTOFMEMORY;
    }

    /* The links stream holds one saved shortcut per item, each behind its size */
    m_pstmLinks = pList->OpenLinksStream(STGM_READ);
    hr = m_pstmLinks ? S_OK : E_FAIL;
    for (int i = 0; SUCCEEDED(hr) && i < GetCount(); i++)
    {
        PPIN_ITEM pItem = GetItem(i);
        LARGE_INTEGER liZero = { 0 };
        LARGE_INTEGER liSkip;
        DWORD cbLink = 0;

        hr = IStream_Read(m_pstmLinks, &cbLink, sizeof(cbLink));
        if (SUCCEEDED(hr))
            hr = m_pstmLinks->Seek(liZero, STREAM_SEEK_CUR, &pItem->uliLinkPos);
        if (SUCCEEDED(hr))
        {
            liSkip.QuadPart = cbLink;
            hr = m_pstmLinks->Seek(liSkip, STREAM_SEEK_CUR, NULL);
        }
        if (SUCCEEDED(hr))
            pItem->cbLink = cbLink;
    }

    /* Without a usable links stream every item simply has no saved link */
    if (FAILED(hr))
    {
        for (int i = 0; i < GetCount(); i++)
            ForgetSavedLink(GetItem(i));
    }
    return S_OK;
}

struct PIN_SAVE_CONTEXT
{
    IStream *pstmPins;
    IStream *pstmLinks;
    CPinStore *pStore;
    HRESULT hr;
    PIDLIST_ABSOLUTE rgpidlFolders[_countof(g_PinFolderCsidls)];

    static INT CALLBACK WriteItem(_In_ PVOID pItem, _In_opt_ PVOID pData);
};

/* Registry streams have no CopyTo, so copy through a buffer */
static
HRESULT
CopyStreamBytes(
    _In_ IStream *pstmFrom,
    _In_ IStream *pstmTo,
    _In_ DWORD cbCopy)
{
    BYTE rgbBuffer[512];
    HRESULT hr = S_OK;

    while (SUCCEEDED(hr) && cbCopy)
    {
        DWORD cbChunk = min(cbCopy, (DWORD)sizeof(rgbBuffer));

        hr = IStream_Read(pstmFrom, rgbBuffer, cbChunk);
        if (SUCCEEDED(hr))
            hr = IStream_Write(pstmTo, rgbBuffer, cbChunk);
        cbCopy -= cbChunk;
    }
    return hr;
}

static
HRESULT
WriteSavedLink(
    _In_ IStream *pstm,
    _In_ IShellLinkW *psl)
{
    CComPtr<IPersistStream> pps;
    LARGE_INTEGER liZero = { 0 };
    LARGE_INTEGER liPos;
    ULARGE_INTEGER uliSize;
    ULARGE_INTEGER uliEnd;
    DWORD cbLink = 0;
    HRESULT hr;

    /* The size goes in front, so reserve it and come back once the link is written */
    hr = pstm->Seek(liZero, STREAM_SEEK_CUR, &uliSize);
    if (SUCCEEDED(hr))
        hr = IStream_Write(pstm, &cbLink, sizeof(cbLink));
    if (SUCCEEDED(hr))
        hr = psl->QueryInterface(IID_PPV_ARG(IPersistStream, &pps));
    if (SUCCEEDED(hr))
        hr = pps->Save(pstm, TRUE);
    if (SUCCEEDED(hr))
        hr = pstm->Seek(liZero, STREAM_SEEK_CUR, &uliEnd);
    if (SUCCEEDED(hr))
    {
        liPos.QuadPart = uliSize.QuadPart;
        hr = pstm->Seek(liPos, STREAM_SEEK_SET, NULL);
    }
    if (SUCCEEDED(hr))
    {
        cbLink = (DWORD)(uliEnd.QuadPart - uliSize.QuadPart - sizeof(cbLink));
        hr = IStream_Write(pstm, &cbLink, sizeof(cbLink));
    }
    if (SUCCEEDED(hr))
    {
        liPos.QuadPart = uliEnd.QuadPart;
        hr = pstm->Seek(liPos, STREAM_SEEK_SET, NULL);
    }
    return hr;
}

INT
CALLBACK
PIN_SAVE_CONTEXT::WriteItem(
    _In_ PVOID pItem,
    _In_opt_ PVOID pData)
{
    PIN_SAVE_CONTEXT *pContext = (PIN_SAVE_CONTEXT *)pData;
    PPIN_ITEM pPin = (PPIN_ITEM)pItem;
    PCUIDLIST_RELATIVE pidlWrite = pPin->pidl;
    BYTE bCsidl = CSIDL_DESKTOP;

    for (UINT i = 0; i < _countof(g_PinFolderCsidls); i++)
    {
        PCUIDLIST_RELATIVE pidlRest;

        if (!pContext->rgpidlFolders[i])
            continue;
        pidlRest = ILFindChild(pContext->rgpidlFolders[i], pPin->pidl);
        if (pidlRest)
        {
            pidlWrite = pidlRest;
            bCsidl = g_PinFolderCsidls[i];
            break;
        }
    }

    pContext->hr = IStream_Write(pContext->pstmPins, &bCsidl, sizeof(bCsidl));
    if (SUCCEEDED(pContext->hr))
        pContext->hr = IStream_WritePidl(pContext->pstmPins, pidlWrite);
    if (FAILED(pContext->hr))
        return FALSE;

    if (pPin->psl)
    {
        pContext->hr = WriteSavedLink(pContext->pstmLinks, pPin->psl);
    }
    else
    {
        /* An unchanged link is copied over from the old stream */
        pContext->hr = IStream_Write(pContext->pstmLinks, &pPin->cbLink, sizeof(pPin->cbLink));
        if (SUCCEEDED(pContext->hr) && pPin->cbLink)
        {
            IStream *pstmOld = pContext->pStore->m_pstmLinks;
            LARGE_INTEGER liPos;

            liPos.QuadPart = pPin->uliLinkPos.QuadPart;
            pContext->hr = pstmOld->Seek(liPos, STREAM_SEEK_SET, NULL);
            if (SUCCEEDED(pContext->hr))
                pContext->hr = CopyStreamBytes(pstmOld, pContext->pstmLinks, pPin->cbLink);
        }
    }
    return SUCCEEDED(pContext->hr);
}

HRESULT
CPinStore::Save(
    _In_ CPinnedListBase *pList)
{
    PIN_SAVE_CONTEXT context = { 0 };
    ULARGE_INTEGER uliZero = { 0 };
    UINT cChanges = 0;

    for (UINT i = 0; i < _countof(g_PinFolderCsidls); i++)
        SHGetFolderLocation(NULL, g_PinFolderCsidls[i], NULL, 0, &context.rgpidlFolders[i]);

    context.pStore = this;
    context.pstmPins = pList->OpenPinStream(STGM_WRITE);
    if (context.pstmPins)
    {
        context.pstmLinks = pList->OpenLinksStream(STGM_WRITE);
        if (context.pstmLinks)
        {
            DSA_EnumCallback(m_hdsa, PIN_SAVE_CONTEXT::WriteItem, &context);
            if (SUCCEEDED(context.hr))
            {
                BYTE bEnd = PIN_STREAM_END;
                context.hr = IStream_Write(context.pstmPins, &bEnd, sizeof(bEnd));
            }

            /* A half written list must not replace the old one */
            if (FAILED(context.hr))
                context.pstmLinks->SetSize(uliZero);
            context.pstmLinks->Release();
        }
        else
        {
            context.hr = E_ACCESSDENIED;
        }

        if (FAILED(context.hr))
            context.pstmPins->SetSize(uliZero);
        context.pstmPins->Release();
    }
    else
    {
        context.hr = E_ACCESSDENIED;
    }

    for (UINT i = 0; i < _countof(g_PinFolderCsidls); i++)
        ILFree(context.rgpidlFolders[i]);

    pList->GetChangeCount(&cChanges);
    pList->SetChangeCount(cChanges + 1);
    if (SUCCEEDED(context.hr))
        pList->SetPinStreamVersion(pList->GetCurrentPinStreamVersion());
    return context.hr;
}

/* The first item that names the same thing, by ID list or by path */
PPIN_ITEM
CPinStore::FindPidl(
    _In_ PCIDLIST_ABSOLUTE pidl,
    _Out_opt_ int *piIndex)
{
    for (int i = 0; i < GetCount(); i++)
    {
        PPIN_ITEM pItem = GetItem(i);

        if (SHCompareIDsFull(SHCIDS_CANONICALONLY, pItem->pidl, pidl, NULL) == S_OK ||
            ILIsEqualByPath(pItem->pidl, pidl))
        {
            if (piIndex)
                *piIndex = i;
            return pItem;
        }
    }
    return NULL;
}

/* Falls back to the application ID, so a pinned program matches any of its shortcuts */
PPIN_ITEM
CPinStore::FindPidlOrAppID(
    _In_ PCIDLIST_ABSOLUTE pidl,
    _Out_opt_ int *piIndex)
{
    PPIN_ITEM pFound = FindPidl(pidl, piIndex);
    PWSTR pszAppID = NULL;

    if (pFound)
        return pFound;

    if (!ILGetHiddenStringAllocW(pidl, HIDDENID_APPID, &pszAppID))
    {
        CComPtr<IApplicationResolver> par;

        if (SUCCEEDED(CoCreateInstance(CLSID_StartMenuCacheAndAppResolver, NULL, CLSCTX_INPROC,
                                       IID_PPV_ARG(IApplicationResolver, &par))))
        {
            GetAppIDFromIDList(par, pidl, &pszAppID, NULL);
        }
    }

    for (int i = 0; pszAppID && i < GetCount(); i++)
    {
        PPIN_ITEM pItem = GetItem(i);
        PWSTR pszItemAppID;
        BOOL fMatch;

        if (!ILGetHiddenStringAllocW(pItem->pidl, HIDDENID_APPID, &pszItemAppID))
            continue;

        fMatch = (CompareStringOrdinal(pszItemAppID, -1, pszAppID, -1, TRUE) == CSTR_EQUAL);
        CoTaskMemFree(pszItemAppID);
        if (fMatch)
        {
            if (piIndex)
                *piIndex = i;
            pFound = pItem;
            break;
        }
    }

    CoTaskMemFree(pszAppID);
    return pFound;
}

/* pidlTo is a new ID list, a 1 based place to move the item to, or NULL to remove it */
HRESULT
CPinStore::ReplacePidl(
    _In_ CPinnedListBase *pList,
    _In_ PCIDLIST_ABSOLUTE pidlFrom,
    _In_opt_ PCIDLIST_ABSOLUTE pidlTo,
    _Outptr_opt_ PIDLIST_ABSOLUTE *ppidlOld)
{
    PPIN_ITEM pItem;
    int iFrom = 0;
    HRESULT hr;

    pItem = FindPidlOrAppID(pidlFrom, &iFrom);
    if (!pItem)
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

    if (ppidlOld)
    {
        *ppidlOld = ILClone(pItem->pidl);
        if (!*ppidlOld)
            return E_OUTOFMEMORY;
    }

    if (!pidlTo)
    {
        WCHAR szSubDir[MAX_PATH];
        WCHAR szBackup[MAX_PATH] = L"";
        PIDLIST_ABSOLUTE pidlBackup = NULL;

        /* The shortcut copy made for the item goes away with it */
        if (SUCCEEDED(pList->GetBackupSubDirectoryName(szSubDir, _countof(szSubDir))))
        {
            CPinLinkFolder linkFolder;

            if (SUCCEEDED(linkFolder.Initialize(szSubDir)) && linkFolder.IsBackupLink(pItem->pidl) == S_OK)
                SHGetPathFromIDListW(pItem->pidl, szBackup);
        }

        if (szBackup[0])
        {
            CComPtr<IBindCtx> pbc;

            if (SUCCEEDED(CreateBindCtxWithParam(PIN_BIND_NO_ALIAS, &pbc)))
                SHParseDisplayName(szBackup, pbc, &pidlBackup, 0, NULL);
        }

        FreePinItemCallback(pItem, NULL);
        DSA_DeleteItem(m_hdsa, iFrom);
        hr = Save(pList);

        if (szBackup[0])
        {
            DeleteFileW(szBackup);
            if (pidlBackup)
                PinSendChangeNotify(SHCNE_DELETE, pidlBackup);
        }
        ILFree(pidlBackup);
        return hr;
    }

    if (IS_INTRESOURCE(pidlTo))
    {
        int iTo = (int)(INT_PTR)pidlTo - 1;
        PIN_ITEM moved = *pItem;

        /* Moving down leaves the gap of the moved item behind, so it lands one place earlier */
        if (iFrom < iTo)
        {
            iTo -= 1;
            MoveMemory(GetItem(iFrom), GetItem(iFrom + 1), (iTo - iFrom) * sizeof(PIN_ITEM));
        }
        else if (iFrom > iTo)
        {
            MoveMemory(GetItem(iTo + 1), GetItem(iTo), (iFrom - iTo) * sizeof(PIN_ITEM));
        }
        *GetItem(iTo) = moved;
        return Save(pList);
    }

    PIDLIST_ABSOLUTE pidlNew = ILClone(pidlTo);
    if (!pidlNew)
        return E_OUTOFMEMORY;

    hr = CacheAppIDInIDList(pidlNew, &pidlNew);
    if (FAILED(hr))
        return hr;

    ILFree(pItem->pidl);
    pItem->pidl = pidlNew;
    PinItemUpdateShellLink(pItem);
    return Save(pList);
}

HRESULT
CPinStore::LoadShellLink(
    _In_ PPIN_ITEM pItem,
    _Outptr_ IShellLinkW **ppsl)
{
    HRESULT hr = S_OK;

    *ppsl = NULL;
    if (!pItem->psl)
    {
        CComPtr<IPersistStream> pps;
        LARGE_INTEGER liPos;

        if (!pItem->cbLink)
            return E_FAIL;

        hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC, IID_PPV_ARG(IPersistStream, &pps));
        if (SUCCEEDED(hr))
        {
            liPos.QuadPart = pItem->uliLinkPos.QuadPart;
            hr = m_pstmLinks->Seek(liPos, STREAM_SEEK_SET, NULL);
        }
        if (SUCCEEDED(hr))
            hr = pps->Load(m_pstmLinks);
        if (SUCCEEDED(hr))
            hr = pps->QueryInterface(IID_PPV_ARG(IShellLinkW, &pItem->psl));
        if (FAILED(hr))
            return hr;
    }

    *ppsl = pItem->psl;
    (*ppsl)->AddRef();
    return S_OK;
}

/* A new or changed item gets a fresh link to save with the list */
HRESULT
PinItemUpdateShellLink(
    _Inout_ PPIN_ITEM pItem)
{
    HRESULT hr;

    ForgetSavedLink(pItem);
    hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC, IID_PPV_ARG(IShellLinkW, &pItem->psl));
    if (FAILED(hr))
        return hr;

    SetLinkFlags(pItem->psl, PIN_SLDF_ALLOW_LINK_TO_LINK, PIN_SLDF_ALLOW_LINK_TO_LINK);
    hr = pItem->psl->SetIDList(pItem->pidl);
    if (FAILED(hr))
        ForgetSavedLink(pItem);
    return hr;
}

/* The User Pinned folder seen through its shell folder, so pinned links get stable ID lists */
static const WCHAR g_szUserPinnedFolder[] = L"::{1f3427c8-5c10-4210-aa03-2ee45287d668}\\";

HRESULT
CPinLinkFolder::GetPinnedFolderIDList(
    _In_ PCWSTR pszLink,
    _Outptr_ PIDLIST_ABSOLUTE *ppidl)
{
    WCHAR szName[MAX_PATH];
    PWSTR pszPinned;
    HRESULT hr;

    *ppidl = NULL;
    hr = SHGetKnownFolderPath(FOLDERID_UserPinned, KF_FLAG_DONT_VERIFY, NULL, &pszPinned);
    if (FAILED(hr))
        return hr;

    if (!(PathComparePaths(pszPinned, pszLink) & PCP_CHILD))
        hr = E_FAIL;
    CoTaskMemFree(pszPinned);
    if (FAILED(hr))
        return hr;

    hr = StringCchCopyW(szName, _countof(szName), g_szUserPinnedFolder);
    if (SUCCEEDED(hr) && !PathAppendW(szName, m_pszSubDir))
        hr = E_FAIL;
    if (SUCCEEDED(hr) && !PathAppendW(szName, PathFindFileNameW(pszLink)))
        hr = E_FAIL;
    if (SUCCEEDED(hr))
    {
        CComPtr<IBindCtx> pbc;
        BIND_OPTS bo = { sizeof(bo), 0, STGM_CREATE, 0 };

        hr = CreateBindCtx(0, &pbc);
        if (SUCCEEDED(hr))
            hr = pbc->SetBindOptions(&bo);
        if (SUCCEEDED(hr))
            hr = SHParseDisplayName(szName, pbc, ppidl, 0, NULL);
    }
    return hr;
}

HRESULT
CPinLinkFolder::GetBackupPath(
    _Outptr_ PWSTR *ppszPath)
{
    PIDLIST_ABSOLUTE pidlPinned;
    WCHAR szPath[MAX_PATH];
    HRESULT hr;

    *ppszPath = NULL;
    hr = SHGetKnownFolderIDList(FOLDERID_UserPinned, KF_FLAG_CREATE | KF_FLAG_NO_ALIAS, NULL, &pidlPinned);
    if (FAILED(hr))
        return hr;

    if (!SHGetPathFromIDListEx(pidlPinned, szPath, _countof(szPath), 2))
        hr = E_OUTOFMEMORY;
    else if (!PathAppendW(szPath, m_pszSubDir))
        hr = E_FAIL;
    ILFree(pidlPinned);

    if (SUCCEEDED(hr) && !PathFileExistsW(szPath) && !CreateDirectoryW(szPath, NULL))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        if (SUCCEEDED(hr))
            hr = E_FAIL;
    }
    if (SUCCEEDED(hr))
        hr = SHStrDupW(szPath, ppszPath);
    return hr;
}

/* S_OK when the item lies in this list's shortcut folder */
HRESULT
CPinLinkFolder::IsBackupLink(
    _In_ PCIDLIST_ABSOLUTE pidl)
{
    CComPtr<IShellItem> psi;
    PIDLIST_ABSOLUTE pidlFolder;
    HRESULT hr;

    hr = SHCreateItemInKnownFolder(FOLDERID_UserPinned, KF_FLAG_DONT_VERIFY, m_pszSubDir, IID_PPV_ARG(IShellItem, &psi));
    if (FAILED(hr))
        return hr;

    hr = SHGetIDListFromObject(psi, &pidlFolder);
    if (FAILED(hr))
        return hr;

    hr = (ILIsParent(pidlFolder, pidl, FALSE) || ILIsParentByPath(pidlFolder, pidl, TRUE)) ? S_OK : S_FALSE;
    ILFree(pidlFolder);
    return hr;
}

HRESULT
CPinLinkFolder::CopyShortcut(
    _In_ PCWSTR pszSource,
    _In_ PCWSTR pszFolder,
    _Outptr_ PIDLIST_ABSOLUTE *ppidl)
{
    WCHAR szFrom[MAX_PATH + 1] = L"";
    WCHAR szTo[MAX_PATH + 1] = L"";
    SHFILEOPSTRUCTW op = { 0 };
    HRESULT hr;

    *ppidl = NULL;

    /* SHFileOperation wants double terminated lists */
    hr = StringCchCopyW(szFrom, MAX_PATH, pszSource);
    if (FAILED(hr))
        return hr;

    /* An existing copy of the same name gets a numbered sibling */
    PathYetAnotherMakeUniqueName(szTo, pszFolder, NULL, PathFindFileNameW(pszSource));

    op.wFunc = FO_COPY;
    op.pFrom = szFrom;
    op.pTo = szTo;
    op.fFlags = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR | FOF_NOERRORUI;
    if (SHFileOperationW(&op) || op.fAnyOperationsAborted)
        return E_FAIL;

    hr = GetPinnedFolderIDList(szTo, ppidl);
    if (FAILED(hr))
        hr = SHParseDisplayName(szTo, NULL, ppidl, 0, NULL);
    return hr;
}

HRESULT
CPinLinkFolder::CreateShortcut(
    _In_ PCIDLIST_ABSOLUTE pidlTarget,
    _In_ PCWSTR pszTarget,
    _In_ SFGAOF sfgaoTarget,
    _In_ PCWSTR pszLink,
    _In_opt_ PCWSTR pszAppID,
    _Outptr_ PIDLIST_ABSOLUTE *ppidl)
{
    CComPtr<IShellLinkW> psl;
    CComPtr<IPersistFile> ppf;
    WCHAR szLink[MAX_PATH];
    WCHAR szFolder[MAX_PATH];
    HRESULT hr;

    *ppidl = NULL;
    hr = StringCchCopyW(szFolder, _countof(szFolder), pszLink);
    if (FAILED(hr))
        return hr;
    PathRemoveFileSpecW(szFolder);
    PathYetAnotherMakeUniqueName(szLink, szFolder, NULL, PathFindFileNameW(pszLink));

    hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC, IID_PPV_ARG(IShellLinkW, &psl));
    if (SUCCEEDED(hr))
        hr = psl->SetIDList(pidlTarget);
    if (FAILED(hr))
        return hr;

    /* Links without a property store cannot carry the ID; the target's own then applies */
    if (pszAppID)
    {
        CComPtr<IPropertyStore> pps;

        if (SUCCEEDED(psl->QueryInterface(IID_PPV_ARG(IPropertyStore, &pps))))
        {
            PROPVARIANT pv;

            pv.vt = VT_LPWSTR;
            pv.pwszVal = const_cast<PWSTR>(pszAppID);
            hr = pps->SetValue(PKEY_AppUserModel_ID, pv);
            if (SUCCEEDED(hr))
                hr = pps->Commit();
            if (FAILED(hr))
                return hr;
        }
    }

    /* A file keeps working from the folder it lives in */
    if ((sfgaoTarget & SFGAO_STREAM) && !(sfgaoTarget & SFGAO_FOLDER))
    {
        WCHAR szWorkDir[MAX_PATH];

        hr = StringCchCopyW(szWorkDir, _countof(szWorkDir), pszTarget);
        if (FAILED(hr))
            return hr;
        PathRemoveFileSpecW(szWorkDir);
        hr = psl->SetWorkingDirectory(szWorkDir);
        if (FAILED(hr))
            return hr;
    }

    hr = psl->QueryInterface(IID_PPV_ARG(IPersistFile, &ppf));
    if (SUCCEEDED(hr))
        hr = ppf->Save(szLink, FALSE);
    if (FAILED(hr))
        return hr;

    hr = GetPinnedFolderIDList(szLink, ppidl);
    if (FAILED(hr))
        hr = SHParseDisplayName(szLink, NULL, ppidl, 0, NULL);
    return hr;
}

/*
 * Pinned programs live on as shortcut copies in User Pinned, so removing the original
 * does not break the pin. Anything that is not such a program is pinned as it is.
 */
HRESULT
CPinLinkFolder::CreateBackupLink(
    _In_ PCIDLIST_ABSOLUTE pidl,
    _Outptr_ PIDLIST_ABSOLUTE *ppidlLink)
{
    WCHAR szTarget[MAX_PATH];
    DWORD sfgao = SFGAO_FILESYSTEM | SFGAO_FOLDER | SFGAO_STREAM | SFGAO_LINK;
    PWSTR pszCachedAppID = NULL;
    BOOL fBackup = FALSE;
    HRESULT hr;

    *ppidlLink = NULL;
    hr = SHGetNameAndFlagsW(pidl, SHGDN_FORPARSING, szTarget, _countof(szTarget), &sfgao);
    if (FAILED(hr))
        return hr;

    if (sfgao & SFGAO_FILESYSTEM)
    {
        CComPtr<IApplicationResolver> par;

        if (SUCCEEDED(CoCreateInstance(CLSID_StartMenuCacheAndAppResolver, NULL, CLSCTX_INPROC,
                                       IID_PPV_ARG(IApplicationResolver, &par))))
        {
            PWSTR pszAppID;
            BOOL fCached;

            if (SUCCEEDED(GetAppIDFromIDList(par, pidl, &pszAppID, &fCached)))
            {
                fBackup = (*pszAppID != UNICODE_NULL);
                if (fBackup && fCached)
                    pszCachedAppID = pszAppID;
                else
                    CoTaskMemFree(pszAppID);
            }
        }
    }

    /* Shortcuts the resolver made for windows are already private copies */
    if (fBackup)
    {
        PIDLIST_ABSOLUTE pidlImplicit;

        if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_ImplicitAppShortcuts, KF_FLAG_CREATE | KF_FLAG_NO_ALIAS,
                                             NULL, &pidlImplicit)))
        {
            if (ILIsParent(pidlImplicit, pidl, FALSE) || ILIsParentByPath(pidlImplicit, pidl, FALSE))
                fBackup = FALSE;
            ILFree(pidlImplicit);
        }
    }

    PWSTR pszFolder = NULL;
    if (!fBackup || FAILED(GetBackupPath(&pszFolder)))
    {
        CoTaskMemFree(pszCachedAppID);
        *ppidlLink = ILClone(pidl);
        return *ppidlLink ? S_OK : E_OUTOFMEMORY;
    }

    if (sfgao & SFGAO_LINK)
    {
        hr = CopyShortcut(szTarget, pszFolder, ppidlLink);
    }
    else
    {
        WCHAR szLink[MAX_PATH];
        WCHAR szFriendly[MAX_PATH];
        DWORD cchFriendly = _countof(szFriendly);
        BOOL fNamed = FALSE;

        hr = StringCchCopyW(szLink, _countof(szLink), pszFolder);
        if (SUCCEEDED(hr))
        {
            /* The program's friendly name makes the nicer shortcut name */
            if (SUCCEEDED(AssocQueryStringW(ASSOCF_INIT_BYEXENAME | ASSOCF_VERIFY, ASSOCSTR_FRIENDLYAPPNAME,
                                            szTarget, NULL, szFriendly, &cchFriendly)) &&
                PathIsFileSpecW(szFriendly) &&
                PathCleanupSpec(szLink, szFriendly) >= 0)
            {
                fNamed = PathAppendW(szLink, szFriendly);
            }
            if (!fNamed && !PathAppendW(szLink, PathFindFileNameW(szTarget)))
                hr = E_FAIL;
        }
        if (SUCCEEDED(hr) && !PathRenameExtensionW(szLink, L".lnk"))
            hr = E_FAIL;
        if (SUCCEEDED(hr))
            hr = CreateShortcut(pidl, szTarget, sfgao, szLink, pszCachedAppID, ppidlLink);
    }

    if (SUCCEEDED(hr))
        PinSendChangeNotify(SHCNE_CREATE, *ppidlLink);

    CoTaskMemFree(pszFolder);
    CoTaskMemFree(pszCachedAppID);
    return hr;
}
