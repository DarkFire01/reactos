/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Recent, frequent and pinned destinations the shell keeps per application
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"
#include "CAutomaticDestinationList.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

#define DESTLIST_STREAM_NAME    L"DestList"
#define DESTLIST_VERSION        1
#define DESTLIST_MAX_ENTRIES    999

/* Usage points above which every entry decays, and the score an entry is dropped under */
#define DESTLIST_DECAY_THRESHOLD    150.0f
#define DESTLIST_MIN_POINTS         0.1f

/* Every this many changes the list drops the entries that scored too low */
#define DESTLIST_GC_INTERVAL        20

/* Low scoring entries that survive one collection */
#define DESTLIST_GC_KEEP            20

/* A use within this time (100ns units) of the previous one adds no point */
#define DESTLIST_DEBOUNCE_DEFAULT   300000000

#define APPDEST_KEY             L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ApplicationDestinations\\"
#define POLICIES_EXPLORER_KEY   L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer"

/* ReactOS ole32 refuses STGM_DIRECT_SWMR with exclusive sharing, which Windows passes too */
#define DESTLIST_STGM           (STGM_READWRITE | STGM_SHARE_EXCLUSIVE)

ULONG CAutoDestList::s_ulDebounceTime = DESTLIST_DEBOUNCE_DEFAULT;
BOOL CAutoDestList::s_fCheckedForCustomDebounceTime = FALSE;

static
BOOL
IsRemoteDestinationsRestricted(VOID)
{
    DWORD dwValue = 0;
    DWORD cbValue = sizeof(dwValue);

    if (SHGetValueW(HKEY_CURRENT_USER, POLICIES_EXPLORER_KEY, L"NoRemoteDestinations", NULL,
                    &dwValue, &cbValue) == ERROR_SUCCESS && dwValue)
    {
        return TRUE;
    }

    cbValue = sizeof(dwValue);
    return SHGetValueW(HKEY_LOCAL_MACHINE, POLICIES_EXPLORER_KEY, L"NoRemoteDestinations", NULL,
                       &dwValue, &cbValue) == ERROR_SUCCESS && dwValue;
}

CAutoDestList::CAutoDestList() :
    m_fDirty(FALSE),
    m_pszPath(NULL),
    m_pstg(NULL),
    m_hdpaEntries(NULL),
    m_pszAppID(NULL),
    m_pszAppPath(NULL)
{
    ZeroMemory(&m_Header, sizeof(m_Header));
    m_Header.dwVersion = DESTLIST_VERSION;

    if (!s_fCheckedForCustomDebounceTime)
    {
        DWORD dwDebounce = 0;
        DWORD cbDebounce = sizeof(dwDebounce);

        s_fCheckedForCustomDebounceTime = TRUE;
        if (SHGetValueW(HKEY_CURRENT_USER, APPDEST_KEY, L"AutomaticDebounce", NULL,
                        &dwDebounce, &cbDebounce) == ERROR_SUCCESS)
        {
            s_ulDebounceTime = dwDebounce;
        }
    }
}

CAutoDestList::~CAutoDestList()
{
    _DestroyEntries();
    if (m_pstg)
        m_pstg->Release();
}

VOID
CAutoDestList::SetOwner(
    _In_ PCWSTR pszPath,
    _In_opt_ PCWSTR pszAppID,
    _In_opt_ PCWSTR pszAppPath)
{
    m_pszPath = pszPath;
    m_pszAppID = pszAppID;
    m_pszAppPath = pszAppPath;
}

VOID
CAutoDestList::FreeEntry(
    _In_opt_ PDESTLIST_ENTRY pEntry)
{
    if (pEntry)
    {
        CoTaskMemFree(pEntry->pszName);
        delete pEntry;
    }
}

int
CALLBACK
CAutoDestList::s_FreeEntryCB(
    _In_ void *p,
    _In_opt_ void *pData)
{
    UNREFERENCED_PARAMETER(pData);

    FreeEntry((PDESTLIST_ENTRY)p);
    return TRUE;
}

VOID
CAutoDestList::_DestroyEntries(VOID)
{
    if (m_hdpaEntries)
    {
        DPA_DestroyCallback(m_hdpaEntries, s_FreeEntryCB, NULL);
        m_hdpaEntries = NULL;
    }
}

/* Forgets every entry; the next Persist deletes the file */
VOID
CAutoDestList::ClearAll(VOID)
{
    _DestroyEntries();
    m_Header.cEntries = 0;
    m_Header.cPinned = 0;
    m_Header.flTotalPoints = 0.0f;
    m_Header.cChanges++;
    m_fDirty = TRUE;
}

PDESTLIST_ENTRY
CAutoDestList::GetEntry(
    _In_ int iEntry) const
{
    return (PDESTLIST_ENTRY)DPA_GetPtr(m_hdpaEntries, iEntry);
}

HRESULT
CAutoDestList::_EnsureStorage(VOID)
{
    HRESULT hr;

    if (m_pstg)
        return S_FALSE;

    hr = StgOpenStorageEx(m_pszPath, DESTLIST_STGM, STGFMT_STORAGE, 0, NULL, NULL,
                          IID_PPV_ARG(IStorage, &m_pstg));
    if (hr == STG_E_DOCFILECORRUPT)
    {
        DeleteFileW(m_pszPath);
        hr = STG_E_FILENOTFOUND;
    }

    if (hr == STG_E_FILENOTFOUND)
    {
        hr = StgCreateStorageEx(m_pszPath, DESTLIST_STGM, STGFMT_STORAGE, 0, NULL, NULL,
                                IID_PPV_ARG(IStorage, &m_pstg));
    }
    return hr;
}

HRESULT
CAutoDestList::_GetDestListStream(
    _Outptr_ IStream **ppstm)
{
    HRESULT hr;

    *ppstm = NULL;
    hr = _EnsureStorage();
    if (FAILED(hr))
        return hr;

    hr = m_pstg->OpenStream(DESTLIST_STREAM_NAME, NULL, DESTLIST_STGM, 0, ppstm);
    if (hr == STG_E_FILENOTFOUND)
        hr = m_pstg->CreateStream(DESTLIST_STREAM_NAME, DESTLIST_STGM, 0, 0, ppstm);
    return hr;
}

/* Every entry keeps its link in a stream named by its ID in hex */
HRESULT
CAutoDestList::_GetEntryStream(
    _In_ ULONGLONG ullEntryId,
    _Outptr_ IStream **ppstm)
{
    WCHAR szName[32];
    HRESULT hr;

    *ppstm = NULL;
    hr = _EnsureStorage();
    if (FAILED(hr))
        return hr;

    StringCchPrintfW(szName, _countof(szName), L"%lx", (ULONG)ullEntryId);
    hr = m_pstg->OpenStream(szName, NULL, DESTLIST_STGM, 0, ppstm);
    if (hr == STG_E_FILENOTFOUND)
        hr = m_pstg->CreateStream(szName, DESTLIST_STGM, 0, 0, ppstm);
    return hr;
}

HRESULT
CAutoDestList::_LoadEntries(
    _In_ IStream *pstm)
{
    HRESULT hr = S_OK;

    for (UINT i = 0; i < m_Header.cEntries && SUCCEEDED(hr); i++)
    {
        PDESTLIST_ENTRY pEntry = new DESTLIST_ENTRY;
        DESTLIST_ENTRY_DATA Check;

        if (!pEntry)
        {
            hr = E_OUTOFMEMORY;
            break;
        }
        pEntry->pszName = NULL;

        hr = IStream_Read(pstm, &pEntry->Data, sizeof(pEntry->Data));
        if (SUCCEEDED(hr))
        {
            Check = pEntry->Data;
            Check.ullChecksum = 0;
            CJumpListCrc64 crc(&Check, sizeof(Check));

            /* A damaged entry is dropped and the rest of the stream read as it comes */
            if (pEntry->Data.flPoints >= 0.0f && pEntry->Data.ullChecksum == crc.m_ullValue)
            {
                hr = JumpListStreamReadString(pstm, &pEntry->pszName);
                if (SUCCEEDED(hr))
                {
                    if (DPA_AppendPtr(m_hdpaEntries, pEntry) == -1)
                        hr = E_OUTOFMEMORY;
                    else
                        pEntry = NULL;
                }
            }
        }
        FreeEntry(pEntry);
    }
    return hr;
}

HRESULT
CAutoDestList::EnsureLoaded(VOID)
{
    CComPtr<IStream> pstm;
    DESTLIST_HEADER Header;
    ULARGE_INTEGER uliSize;
    HRESULT hr;

    if (m_hdpaEntries)
        return S_OK;

    if (!m_pstg && !PathFileExistsW(m_pszPath))
    {
        m_hdpaEntries = DPA_Create(4);
        return S_OK;
    }

    hr = _GetDestListStream(&pstm);
    if (FAILED(hr))
        return hr;

    uliSize.QuadPart = ~0ULL;
    hr = IStream_Size(pstm, &uliSize);
    if (FAILED(hr))
        return hr;

    BOOL fValid = FALSE;
    if (uliSize.LowPart > sizeof(Header))
    {
        hr = IStream_Read(pstm, &Header, sizeof(Header));
        if (FAILED(hr))
            return hr;

        if (Header.dwVersion != DESTLIST_VERSION ||
            Header.cEntries - 1 > DESTLIST_MAX_ENTRIES - 1)
        {
            return E_INVALIDARG;
        }

        ULONGLONG cbEntries = (ULONGLONG)Header.cEntries * sizeof(DESTLIST_ENTRY_DATA) + sizeof(Header);
        fValid = (cbEntries <= MAXDWORD && uliSize.LowPart > (DWORD)cbEntries);
    }

    /* A list too short to hold its own entries is thrown away */
    if (!fValid)
    {
        _DestroyEntries();
        pstm.Release();
        if (m_pstg)
        {
            m_pstg->Release();
            m_pstg = NULL;
        }
        DeleteFileW(m_pszPath);
        return S_FALSE;
    }

    m_hdpaEntries = DPA_Create(Header.cEntries);
    if (!m_hdpaEntries)
        return E_OUTOFMEMORY;

    m_Header = Header;
    hr = _LoadEntries(pstm);
    if (SUCCEEDED(hr))
        m_fDirty = FALSE;
    m_Header.cEntries = DPA_GetPtrCount(m_hdpaEntries);
    return hr;
}

HRESULT
CAutoDestList::Persist(VOID)
{
    CComPtr<IStream> pstm;
    ULARGE_INTEGER uliSize;
    HRESULT hr;

    if (!m_Header.cEntries)
    {
        /* An empty list leaves no file behind */
        if (m_pstg)
        {
            m_pstg->Commit(STGC_DANGEROUSLYCOMMITMERELYTODISKCACHE);
            m_pstg->Release();
            m_pstg = NULL;
        }
        if (!DeleteFileW(m_pszPath))
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
            return SUCCEEDED(hr) ? E_FAIL : hr;
        }
        return S_OK;
    }

    hr = _EnsureStorage();
    if (FAILED(hr) || !m_fDirty)
        return hr;

    hr = _GetDestListStream(&pstm);
    if (FAILED(hr))
        return hr;

    ULONGLONG cbStream = (ULONGLONG)m_Header.cEntries * sizeof(DESTLIST_ENTRY_DATA) + sizeof(m_Header);
    if (cbStream > MAXDWORD)
        return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);

    uliSize.QuadPart = cbStream;
    hr = pstm->SetSize(uliSize);
    if (SUCCEEDED(hr))
        hr = IStream_Write(pstm, &m_Header, sizeof(m_Header));

    for (UINT i = 0; SUCCEEDED(hr) && i < m_Header.cEntries; i++)
    {
        PDESTLIST_ENTRY pEntry = GetEntry(i);

        pEntry->Data.ullChecksum = 0;
        CJumpListCrc64 crc(&pEntry->Data, sizeof(pEntry->Data));
        pEntry->Data.ullChecksum = crc.m_ullValue;

        hr = IStream_Write(pstm, &pEntry->Data, sizeof(pEntry->Data));
        if (SUCCEEDED(hr))
            hr = JumpListStreamWriteString(pstm, pEntry->pszName);
    }

    if (SUCCEEDED(hr))
        hr = pstm->Commit(STGC_DANGEROUSLYCOMMITMERELYTODISKCACHE);
    if (SUCCEEDED(hr))
        hr = m_pstg->Commit(STGC_DANGEROUSLYCOMMITMERELYTODISKCACHE);
    return hr;
}

HRESULT
CAutoDestList::_GetMatchingEntryIndex(
    _In_ PCWSTR pszName,
    _Out_ int *piEntry)
{
    for (UINT i = 0; i < m_Header.cEntries; i++)
    {
        if (CompareStringOrdinal(pszName, -1, GetEntry(i)->pszName, -1, TRUE) == CSTR_EQUAL)
        {
            *piEntry = i;
            return S_OK;
        }
    }
    return E_INVALIDARG;
}

/*
 * Finds the entry for an item by its name. Windows also matches moved files through their
 * link tracking IDs; ReactOS links carry none, so only the name is compared.
 */
HRESULT
CAutoDestList::GetMatchingEntryIndex(
    _In_ IUnknown *punk,
    _In_ PCWSTR pszName,
    _Out_ int *piEntry)
{
    HRESULT hr;

    UNREFERENCED_PARAMETER(punk);

    *piEntry = 0;
    hr = EnsureLoaded();
    if (SUCCEEDED(hr))
        hr = _GetMatchingEntryIndex(pszName, piEntry);
    return hr;
}

HRESULT
CAutoDestList::_SaveLink(
    _In_ ULONGLONG ullEntryId,
    _In_ IShellLinkW *psl)
{
    CComPtr<IPersistStream> pps;
    CComPtr<IStream> pstm;
    HRESULT hr;

    hr = _EnsureStorage();
    if (SUCCEEDED(hr))
        hr = _GetEntryStream(ullEntryId, &pstm);
    if (SUCCEEDED(hr))
        hr = psl->QueryInterface(IID_PPV_ARG(IPersistStream, &pps));
    if (SUCCEEDED(hr))
        hr = pps->Save(pstm, TRUE);
    if (SUCCEEDED(hr))
        hr = pstm->Commit(STGC_DANGEROUSLYCOMMITMERELYTODISKCACHE);
    return hr;
}

HRESULT
CAutoDestList::_LoadItemLink(
    _In_ const DESTLIST_ENTRY *pEntry,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    CComPtr<IStream> pstm;
    CComPtr<IPersistStream> pps;
    HRESULT hr;

    *ppv = NULL;
    hr = _GetEntryStream(pEntry->Data.ullEntryId, &pstm);
    if (FAILED(hr))
        return hr;

    hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARG(IPersistStream, &pps));
    if (SUCCEEDED(hr))
        hr = pps->Load(pstm);
    if (SUCCEEDED(hr))
        hr = pps->QueryInterface(riid, ppv);
    return hr;
}

HRESULT
CAutoDestList::AddItem(
    _In_ IUnknown *punk,
    _In_ PCWSTR pszName,
    _In_ BOOL fAddPoint,
    _Out_ int *piEntry)
{
    CComPtr<IShellLinkW> psl;
    CComPtr<IShellItem> psi;
    PDESTLIST_ENTRY pEntry;
    HRESULT hr;

    hr = EnsureLoaded();
    *piEntry = 0;
    if (FAILED(hr))
        return hr;

    if (!m_hdpaEntries)
    {
        m_hdpaEntries = DPA_Create(4);
        if (!m_hdpaEntries)
            return E_OUTOFMEMORY;
    }

    pEntry = new DESTLIST_ENTRY;
    if (!pEntry)
        return E_OUTOFMEMORY;
    ZeroMemory(pEntry, sizeof(*pEntry));

    pEntry->Data.flPoints = fAddPoint ? 1.0f : 0.0f;
    if (fAddPoint)
        GetSystemTimeAsFileTime(&pEntry->Data.ftLastAccess);
    pEntry->Data.iPinPos = DESTLIST_NOT_PINNED;
    pEntry->Data.ullEntryId = ++m_Header.ullLastEntryId;

    hr = SHStrDupW(pszName, &pEntry->pszName);
    if (SUCCEEDED(hr))
    {
        /* Plain items are kept as a link the shell makes for them */
        if (SUCCEEDED(punk->QueryInterface(IID_PPV_ARG(IShellItem, &psi))))
            hr = CreateLinkForItem(psi, &psl, TRUE);
        else if (FAILED(punk->QueryInterface(IID_PPV_ARG(IShellLinkW, &psl))))
            hr = E_INVALIDARG;
    }

    if (SUCCEEDED(hr))
        hr = _SaveLink(pEntry->Data.ullEntryId, psl);

    if (SUCCEEDED(hr))
    {
        if (DPA_InsertPtr(m_hdpaEntries, 0, pEntry) == -1)
            hr = E_OUTOFMEMORY;
    }

    if (FAILED(hr))
    {
        FreeEntry(pEntry);
        return hr;
    }

    m_Header.cChanges++;
    m_Header.cEntries++;
    m_Header.flTotalPoints += pEntry->Data.flPoints;
    m_fDirty = TRUE;
    return _DecayPoints();
}

/*
 * Scales the points down once the total passes the threshold and, every few changes,
 * drops the lowest scoring entries. The newest entry takes part in neither.
 */
HRESULT
CAutoDestList::_DecayPoints(VOID)
{
    BOOL fCollect = (m_Header.cChanges % DESTLIST_GC_INTERVAL) == 0;
    BOOL fScale = m_Header.flTotalPoints > DESTLIST_DECAY_THRESHOLD;
    HRESULT hr = S_FALSE;
    UINT cLow = 0;

    if (!fScale && !fCollect)
        return hr;

    hr = S_OK;
    FLOAT flScale = ((m_Header.flTotalPoints - DESTLIST_DECAY_THRESHOLD) + 1.0f) /
                    (m_Header.flTotalPoints - GetEntry(0)->Data.flPoints);
    FLOAT flTotal = 0.0f;

    for (UINT i = 1; i < m_Header.cEntries; i++)
    {
        PDESTLIST_ENTRY pEntry = GetEntry(i);

        if (fScale)
            pEntry->Data.flPoints -= pEntry->Data.flPoints * flScale;
        flTotal += pEntry->Data.flPoints;

        /* Windows 7 applies the collection to pinned entries */
        if (pEntry->Data.iPinPos != DESTLIST_NOT_PINNED &&
            pEntry->Data.flPoints < DESTLIST_MIN_POINTS &&
            fCollect)
        {
            cLow++;
            if (cLow > DESTLIST_GC_KEEP)
            {
                flTotal -= pEntry->Data.flPoints;
                hr = RemoveMatchingEntry(i);
                i -= 1;
            }
        }
    }
    m_Header.flTotalPoints = flTotal;
    return hr;
}

HRESULT
CAutoDestList::AddPoints(
    _In_ int iEntry)
{
    PDESTLIST_ENTRY pEntry = GetEntry(iEntry);
    ULARGE_INTEGER uliNext;
    FILETIME ftNow;
    FILETIME ftNext;

    GetSystemTimeAsFileTime(&ftNow);
    uliNext.LowPart = pEntry->Data.ftLastAccess.dwLowDateTime;
    uliNext.HighPart = pEntry->Data.ftLastAccess.dwHighDateTime;
    uliNext.QuadPart += s_ulDebounceTime;
    ftNext.dwLowDateTime = uliNext.LowPart;
    ftNext.dwHighDateTime = uliNext.HighPart;

    if (CompareFileTime(&ftNext, &ftNow) > 0)
        return S_FALSE;

    pEntry->Data.flPoints += 1.0f;
    pEntry->Data.ftLastAccess = ftNow;

    /* The used entry moves to the front */
    if (iEntry)
    {
        DPA_DeletePtr(m_hdpaEntries, iEntry);
        if (DPA_InsertPtr(m_hdpaEntries, 0, pEntry) == -1)
            FreeEntry(pEntry);
    }

    m_fDirty = TRUE;
    return _DecayPoints();
}

HRESULT
CAutoDestList::RemoveMatchingEntry(
    _In_ int iEntry)
{
    WCHAR szName[32];
    HRESULT hr;

    if (iEntry < 0 || (UINT)iEntry >= m_Header.cEntries)
        return E_INVALIDARG;

    PDESTLIST_ENTRY pEntry = GetEntry(iEntry);
    if (pEntry->Data.iPinPos != DESTLIST_NOT_PINNED)
        m_Header.cPinned -= 1;
    m_Header.cEntries -= 1;
    m_Header.cChanges++;
    m_Header.flTotalPoints -= pEntry->Data.flPoints;
    m_fDirty = TRUE;

    hr = _EnsureStorage();
    if (SUCCEEDED(hr))
    {
        StringCchPrintfW(szName, _countof(szName), L"%lx", (ULONG)pEntry->Data.ullEntryId);
        hr = m_pstg->DestroyElement(szName);
    }

    FreeEntry((PDESTLIST_ENTRY)DPA_DeletePtr(m_hdpaEntries, iEntry));
    return hr;
}

HRESULT
CAutoDestList::UnPin(
    _In_ int iEntry,
    _In_ BOOL fKeep)
{
    PDESTLIST_ENTRY pEntry = GetEntry(iEntry);
    int iPinPos = pEntry->Data.iPinPos;

    if (iPinPos == DESTLIST_NOT_PINNED)
        return E_INVALIDARG;

    m_Header.cPinned -= 1;
    m_fDirty = TRUE;

    /* The pinned items after it move up */
    for (UINT i = 0; i < m_Header.cEntries; i++)
    {
        PDESTLIST_ENTRY pOther = GetEntry(i);

        if (pOther->Data.iPinPos > iPinPos)
            pOther->Data.iPinPos -= 1;
    }
    pEntry->Data.iPinPos = DESTLIST_NOT_PINNED;

    /* An entry that was only there because it was pinned goes away with the pin */
    if (!fKeep && pEntry->Data.flPoints < DESTLIST_MIN_POINTS)
        return RemoveMatchingEntry(iEntry);
    return S_OK;
}

HRESULT
CAutoDestList::PinAt(
    _In_ int iEntry,
    _In_ int iPinPos)
{
    PDESTLIST_ENTRY pEntry = GetEntry(iEntry);

    m_fDirty = TRUE;
    if (pEntry->Data.iPinPos != DESTLIST_NOT_PINNED)
    {
        if (pEntry->Data.iPinPos < iPinPos && iPinPos)
            iPinPos -= 1;
        UnPin(iEntry, TRUE);
    }

    /* Make room; the item pushed past the last slot is no longer pinned */
    for (UINT i = 0; i < m_Header.cEntries; i++)
    {
        PDESTLIST_ENTRY pOther = GetEntry(i);
        int iOther = pOther->Data.iPinPos;

        if (iOther == DESTLIST_NOT_PINNED || iOther < iPinPos)
            continue;

        if (iOther == JUMPLIST_MAX_PINNED)
        {
            pOther->Data.iPinPos = DESTLIST_NOT_PINNED;
            m_Header.cPinned -= 1;
        }
        else
        {
            pOther->Data.iPinPos = iOther + 1;
        }
    }

    m_Header.cPinned++;
    pEntry->Data.iPinPos = iPinPos;
    return S_OK;
}

/* Orders pinned entries by position, recent ones newest first, frequent ones by score */
int
CALLBACK
CAutoDestList::s_SortEntryCB(
    _In_ void *p1,
    _In_ void *p2,
    _In_ LPARAM lParam)
{
    const DESTLIST_ENTRY *pEntry1 = (const DESTLIST_ENTRY *)p1;
    const DESTLIST_ENTRY *pEntry2 = (const DESTLIST_ENTRY *)p2;

    switch (lParam)
    {
        case DLT_PINNED:
        {
            int iDiff = pEntry1->Data.iPinPos - pEntry2->Data.iPinPos;

            if (!iDiff)
                return 0;
            if (pEntry1->Data.iPinPos != DESTLIST_NOT_PINNED && pEntry2->Data.iPinPos != DESTLIST_NOT_PINNED)
                return iDiff;
            return (pEntry1->Data.iPinPos == DESTLIST_NOT_PINNED) ? 1 : -1;
        }

        case DLT_RECENT:
            return CompareFileTime(&pEntry2->Data.ftLastAccess, &pEntry1->Data.ftLastAccess);

        case DLT_FREQUENT:
            if (pEntry1->Data.flPoints > pEntry2->Data.flPoints)
                return -1;
            if (pEntry2->Data.flPoints > pEntry1->Data.flPoints)
                return 1;
            return 0;
    }
    return 0;
}

HRESULT
CAutoDestList::SetData(
    _In_ int iEntry,
    _In_opt_ const FLOAT *pflPoints,
    _In_opt_ const FILETIME *pftLastAccess)
{
    PDESTLIST_ENTRY pEntry = GetEntry(iEntry);
    HRESULT hr = S_OK;

    if (pflPoints)
    {
        /* The new score may not push the total past the decay threshold */
        if ((m_Header.flTotalPoints + *pflPoints) - pEntry->Data.flPoints >= DESTLIST_DECAY_THRESHOLD)
            return E_INVALIDARG;

        m_Header.flTotalPoints = (m_Header.flTotalPoints - pEntry->Data.flPoints) + *pflPoints;
        pEntry->Data.flPoints = *pflPoints;
        m_fDirty = TRUE;
    }

    if (pftLastAccess)
    {
        pEntry->Data.ftLastAccess = *pftLastAccess;
        m_fDirty = TRUE;
    }

    if (m_hdpaEntries)
        DPA_Sort(m_hdpaEntries, s_SortEntryCB, DLT_RECENT);
    return hr;
}

HRESULT
CAutoDestList::GetItemsMatchingLinks(
    _In_ DESTLISTTYPE type,
    _In_ UINT cMax,
    _In_ IObjectCollection *poc)
{
    HDPA hdpaSorted;
    HRESULT hr;

    hr = EnsureLoaded();
    if (FAILED(hr))
        return hr;

    hr = S_OK;
    if (m_hdpaEntries)
    {
        /* Sort a copy so the stored order stays most recent first */
        hdpaSorted = DPA_Create(4);
        for (int i = 0; hdpaSorted && i < DPA_GetPtrCount(m_hdpaEntries); i++)
        {
            if (DPA_AppendPtr(hdpaSorted, DPA_GetPtr(m_hdpaEntries, i)) == -1)
            {
                DPA_Destroy(hdpaSorted);
                hdpaSorted = NULL;
            }
        }

        if (!hdpaSorted)
            hr = E_OUTOFMEMORY;
        else if (type != DLT_RECENT && !DPA_Sort(hdpaSorted, s_SortEntryCB, type))
            hr = E_OUTOFMEMORY;

        int cSorted = hdpaSorted ? DPA_GetPtrCount(hdpaSorted) : 0;
        if (!cMax)
            cMax = cSorted;

        BOOL fNoRemote = IsRemoteDestinationsRestricted();
        for (int i = 0; i < cSorted && (UINT)i < cMax && SUCCEEDED(hr); i++)
        {
            PDESTLIST_ENTRY pEntry = (PDESTLIST_ENTRY)DPA_GetPtr(hdpaSorted, i);
            CComPtr<IShellLinkW> psl;
            CComPtr<IUnknown> punkItem;

            if (type == DLT_PINNED && pEntry->Data.iPinPos < 0)
                continue;

            if (FAILED(_LoadItemLink(pEntry, IID_PPV_ARG(IShellLinkW, &psl))))
                continue;

            if (fNoRemote)
            {
                PIDLIST_ABSOLUTE pidl;
                WCHAR szPath[MAX_PATH];
                BOOL fRemote = FALSE;

                if (psl->GetIDList(&pidl) == S_OK)
                {
                    fRemote = SHGetPathFromIDListEx(pidl, szPath, _countof(szPath), GPFIDL_UNCPRINTER) &&
                              PathIsNetworkPathW(szPath);
                    CoTaskMemFree(pidl);
                }
                if (fRemote)
                    continue;
            }

            /* Links the shell made hand back the item itself */
            if (IsDestListLink(psl))
                hr = CreateItemFromLink(psl, IID_PPV_ARG(IUnknown, &punkItem));
            else
                hr = psl->QueryInterface(IID_PPV_ARG(IUnknown, &punkItem));

            if (SUCCEEDED(hr))
                hr = poc->AddObject(punkItem);
        }

        if (hdpaSorted)
            DPA_Destroy(hdpaSorted);
    }

    /* Do not keep the file open between enumerations */
    if (m_pstg)
    {
        m_pstg->Release();
        m_pstg = NULL;
    }
    return hr;
}

/* Re-points a resolved link at the item it found; S_FALSE from Resolve means canceled */
HRESULT
CAutoDestList::_ResolveAndBindLinkToAssoc(
    _In_ IShellLinkW *psl,
    _In_opt_ HWND hwnd,
    _In_ DWORD dwFlags)
{
    CComPtr<IShellItem> psi;
    PIDLIST_ABSOLUTE pidl;
    HRESULT hr;

    hr = psl->Resolve(hwnd, dwFlags);
    if (hr != S_OK)
        return hr;

    hr = CreateItemFromLink(psl, IID_PPV_ARG(IShellItem, &psi));
    if (SUCCEEDED(hr))
        hr = MakeDestinationItemForApp(psi, NULL, m_pszAppID, 0, m_pszAppPath, NULL);
    if (FAILED(hr))
        return hr;

    hr = SHGetIDListFromObject(psi, &pidl);
    if (SUCCEEDED(hr))
    {
        psl->SetIDList(NULL);
        hr = psl->SetIDList(pidl);
        if (SUCCEEDED(hr))
            hr = MarkDestListLink(psl);
        CoTaskMemFree(pidl);
    }
    return hr;
}

HRESULT
CAutoDestList::ResolveDestination(
    _In_opt_ HWND hwnd,
    _In_ DWORD dwFlags,
    _In_ int iEntry,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    CComPtr<IShellLinkW> psl;
    CComPtr<IShellItem> psi;
    PDESTLIST_ENTRY pEntry;
    PWSTR pszName = NULL;
    HRESULT hr;

    *ppv = NULL;
    hr = EnsureLoaded();
    if (FAILED(hr))
        return hr;

    pEntry = GetEntry(iEntry);
    hr = _LoadItemLink(pEntry, IID_PPV_ARG(IShellLinkW, &psl));
    if (FAILED(hr))
        return hr;

    hr = _ResolveAndBindLinkToAssoc(psl, hwnd, dwFlags);
    if (hr != S_OK)
        return (hr == S_FALSE) ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : hr;

    hr = _SaveLink(pEntry->Data.ullEntryId, psl);
    if (SUCCEEDED(hr))
        hr = CreateItemFromLink(psl, IID_PPV_ARG(IShellItem, &psi));
    if (FAILED(hr))
        return hr;

    if (FAILED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszName)))
    {
        hr = psi->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &pszName);
        if (FAILED(hr))
            return hr;
    }

    /* Resolving to the same item changes nothing and counts as canceled */
    hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    if (CompareStringOrdinal(pszName, -1, pEntry->pszName, -1, TRUE) != CSTR_EQUAL)
    {
        int iOther;

        /* Entries already naming the new item fold into this one */
        while (SUCCEEDED(_GetMatchingEntryIndex(pszName, &iOther)) &&
               SUCCEEDED(RemoveMatchingEntry(iOther)))
        {
        }

        CoTaskMemFree(pEntry->pszName);
        pEntry->pszName = pszName;
        pszName = NULL;
        hr = psi->QueryInterface(IID_IUnknown, ppv);
        m_fDirty = TRUE;
    }
    CoTaskMemFree(pszName);
    return hr;
}

CAutomaticDestinationList::CAutomaticDestinationList() :
    m_pList(NULL),
    m_pszAppID(NULL),
    m_pszAppPath(NULL),
    m_hrInit(S_OK)
{
    m_szAppHash[0] = UNICODE_NULL;
    m_szListPath[0] = UNICODE_NULL;
}

CAutomaticDestinationList::~CAutomaticDestinationList()
{
    CoTaskMemFree(m_pszAppPath);
    CoTaskMemFree(m_pszAppID);
    _ReleaseList();
}

VOID
CAutomaticDestinationList::_ReleaseList(VOID)
{
    delete m_pList;
    m_pList = NULL;
}

HRESULT
STDMETHODCALLTYPE
CAutomaticDestinationList::Initialize(
    _In_ PCWSTR pszAppID,
    _In_opt_ PCWSTR pszAppPath,
    _In_opt_ PCWSTR pszListPath)
{
    HRESULT hr = S_OK;

    _ReleaseList();
    CoTaskMemFree(m_pszAppID);
    m_pszAppID = NULL;
    CoTaskMemFree(m_pszAppPath);
    m_pszAppPath = NULL;
    m_szListPath[0] = UNICODE_NULL;

    if (pszListPath && *pszListPath)
        hr = StringCchCopyW(m_szListPath, _countof(m_szListPath), pszListPath);

    if (SUCCEEDED(hr) && pszAppID)
    {
        hr = SHStrDupW(pszAppID, &m_pszAppID);
        if (SUCCEEDED(hr) && pszAppPath)
            hr = SHStrDupW(pszAppPath, &m_pszAppPath);
        if (SUCCEEDED(hr))
            hr = HashAppID(pszAppID, m_szAppHash, _countof(m_szAppHash));
    }

    m_hrInit = hr;
    return hr;
}

/* The list file: the application's hash in the AutomaticDestinations folder */
HRESULT
CAutomaticDestinationList::_EnsureDirectory(VOID)
{
    HRESULT hr;

    if (m_szListPath[0])
        return S_OK;

    hr = GetDestinationsPath(DESTINATIONS_AUTOMATIC, m_szListPath);
    if (SUCCEEDED(hr) && !PathAppendW(m_szListPath, m_szAppHash))
        hr = E_FAIL;
    if (SUCCEEDED(hr))
        hr = StringCchCatW(m_szListPath, _countof(m_szListPath), L".automaticDestinations-ms");
    if (FAILED(hr))
        m_szListPath[0] = UNICODE_NULL;
    return hr;
}

/* Entry name of an item: its file system path or parsing name, or a hash of a link */
HRESULT
CAutomaticDestinationList::_GetItemDisplayName(
    _In_ IUnknown *punk,
    _Outptr_ PWSTR *ppszName)
{
    CComPtr<IShellItem> psi;
    CComPtr<IShellLinkW> psl;
    WCHAR szPath[MAX_PATH];
    HRESULT hr;

    *ppszName = NULL;
    if (SUCCEEDED(punk->QueryInterface(IID_PPV_ARG(IShellItem, &psi))))
    {
        hr = psi->GetDisplayName(SIGDN_FILESYSPATH, ppszName);
        if (FAILED(hr))
            hr = psi->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, ppszName);
        return hr;
    }

    hr = punk->QueryInterface(IID_PPV_ARG(IShellLinkW, &psl));
    if (FAILED(hr))
        return hr;

    if (psl->GetPath(szPath, _countof(szPath), NULL, SLGP_RAWPATH) != S_OK)
        return E_INVALIDARG;
    return DisplayNameFromLink(psl, ppszName, szPath);
}

HRESULT
CAutomaticDestinationList::_EnsureItemGetIndex(
    _In_ IUnknown *punk,
    _Inout_ CAutoDestList *pList,
    _In_ BOOL fAddPoint,
    _Out_ int *piEntry,
    _Out_opt_ BOOL *pfAdded)
{
    CComHeapPtr<WCHAR> pszName;
    HRESULT hr;

    *piEntry = 0;
    if (pfAdded)
        *pfAdded = FALSE;

    hr = _EnsureDirectory();
    if (FAILED(hr))
        return hr;

    pList->SetOwner(m_szListPath, m_pszAppID, m_pszAppPath);
    hr = _GetItemDisplayName(punk, &pszName);
    if (FAILED(hr))
        return hr;

    hr = pList->GetMatchingEntryIndex(punk, pszName, piEntry);
    if (FAILED(hr))
    {
        hr = pList->AddItem(punk, pszName, fAddPoint, piEntry);
        if (SUCCEEDED(hr) && pfAdded)
            *pfAdded = TRUE;
    }
    return hr;
}

HRESULT
CAutomaticDestinationList::_GetIndex(
    _In_ IUnknown *punk,
    _Inout_ CAutoDestList *pList,
    _Out_ int *piEntry)
{
    CComHeapPtr<WCHAR> pszName;
    HRESULT hr;

    *piEntry = 0;
    hr = _EnsureDirectory();
    if (FAILED(hr))
        return hr;

    pList->SetOwner(m_szListPath, m_pszAppID, m_pszAppPath);
    hr = _GetItemDisplayName(punk, &pszName);
    if (SUCCEEDED(hr))
        hr = pList->GetMatchingEntryIndex(punk, pszName, piEntry);
    return hr;
}

HRESULT
STDMETHODCALLTYPE
CAutomaticDestinationList::HasList(
    _Out_ BOOL *pfHasList)
{
    HRESULT hr;

    *pfHasList = FALSE;
    if (FAILED(m_hrInit))
        return E_UNEXPECTED;

    hr = _EnsureDirectory();
    if (FAILED(hr))
        return hr;

    UINT cItems = ReadStartPageSettingBounded(L"Start_JumpListItems", JUMPLIST_DEFAULT_ITEMS, 0,
                                              JUMPLIST_MAX_SETTING_ITEMS);
    if (IsRecentDocsEnabled() && cItems)
    {
        *pfHasList = PathFileExistsW(m_szListPath);
        return hr;
    }

    /* Without recent items only pinned ones make a list */
    CComPtr<IObjectCollection> poc;
    if (SUCCEEDED(GetList(DLT_PINNED, 1, IID_PPV_ARG(IObjectCollection, &poc))))
    {
        UINT cObjects = 0;

        if (SUCCEEDED(poc->GetCount(&cObjects)) && cObjects)
            *pfHasList = TRUE;
    }
    return hr;
}

HRESULT
STDMETHODCALLTYPE
CAutomaticDestinationList::GetList(
    _In_ DESTLISTTYPE type,
    _In_ int cMaxCount,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    CComPtr<IObjectCollection> poc;
    HRESULT hr;

    *ppv = NULL;
    if (FAILED(m_hrInit))
        return E_UNEXPECTED;

    hr = _EnsureDirectory();
    if (FAILED(hr))
        return hr;

    if (!m_pList)
    {
        m_pList = new CAutoDestList;
        if (!m_pList)
            return E_OUTOFMEMORY;
    }
    m_pList->SetOwner(m_szListPath, m_pszAppID, m_pszAppPath);

    hr = CreateEnumerableObjectCollection(cMaxCount, IID_PPV_ARG(IObjectCollection, &poc));
    if (SUCCEEDED(hr))
        hr = m_pList->GetItemsMatchingLinks(type, cMaxCount, poc);
    if (SUCCEEDED(hr))
        hr = poc->QueryInterface(riid, ppv);
    return hr;
}

HRESULT
STDMETHODCALLTYPE
CAutomaticDestinationList::AddUsagePoint(
    _In_ IUnknown *punk)
{
    CAutoDestList List;
    BOOL fAdded;
    int iEntry;
    HRESULT hr;

    if (FAILED(m_hrInit))
        return E_UNEXPECTED;

    _ReleaseList();
    hr = _EnsureItemGetIndex(punk, &List, TRUE, &iEntry, &fAdded);

    /* A new entry already starts with its first point */
    if (SUCCEEDED(hr) && !fAdded)
        hr = List.AddPoints(iEntry);
    if (SUCCEEDED(hr))
        hr = List.Persist();
    if (SUCCEEDED(hr))
        JumpListCounterIncrement();
    return hr;
}

HRESULT
STDMETHODCALLTYPE
CAutomaticDestinationList::PinItem(
    _In_ IUnknown *punk,
    _In_ int iPinPos)
{
    CAutoDestList List;
    int iEntry;
    HRESULT hr;

    if (FAILED(m_hrInit))
        return E_UNEXPECTED;

    _ReleaseList();
    hr = _EnsureItemGetIndex(punk, &List, FALSE, &iEntry, NULL);
    if (FAILED(hr))
        return hr;

    if (iPinPos == DESTLIST_PIN_APPEND)
    {
        PDESTLIST_ENTRY pEntry = List.GetEntry(iEntry);

        hr = E_INVALIDARG;
        if (List.m_Header.cPinned < JUMPLIST_MAX_PINNED && pEntry->Data.iPinPos == DESTLIST_NOT_PINNED)
        {
            List.m_fDirty = TRUE;
            pEntry->Data.iPinPos = List.m_Header.cPinned++;
            hr = S_OK;
        }
    }
    else if (iPinPos == DESTLIST_PIN_REMOVE)
    {
        hr = List.UnPin(iEntry, FALSE);
    }
    else if (iPinPos >= 0 && iPinPos <= JUMPLIST_MAX_PINNED)
    {
        hr = List.PinAt(iEntry, iPinPos);
    }
    else
    {
        return E_INVALIDARG;
    }

    if (SUCCEEDED(hr))
        hr = List.Persist();
    if (SUCCEEDED(hr))
        JumpListCounterIncrement();
    return hr;
}

HRESULT
STDMETHODCALLTYPE
CAutomaticDestinationList::IsPinned(
    _In_ IUnknown *punk,
    _Out_ int *piPinPos)
{
    CAutoDestList List;
    int iEntry;
    HRESULT hr;

    if (FAILED(m_hrInit))
        return E_UNEXPECTED;

    hr = _GetIndex(punk, &List, &iEntry);
    if (FAILED(hr))
        return hr;

    int iPinPos = List.GetEntry(iEntry)->Data.iPinPos;
    if (iPinPos == DESTLIST_NOT_PINNED)
        return E_FAIL;

    *piPinPos = iPinPos;
    return S_OK;
}

HRESULT
STDMETHODCALLTYPE
CAutomaticDestinationList::RemoveDestination(
    _In_ IUnknown *punk)
{
    CAutoDestList List;
    int iEntry;
    HRESULT hr;

    if (FAILED(m_hrInit))
        return E_UNEXPECTED;

    _ReleaseList();
    hr = _GetIndex(punk, &List, &iEntry);
    if (SUCCEEDED(hr))
        hr = List.RemoveMatchingEntry(iEntry);
    if (SUCCEEDED(hr))
        hr = List.Persist();
    if (SUCCEEDED(hr))
        JumpListCounterIncrement();
    return hr;
}

HRESULT
STDMETHODCALLTYPE
CAutomaticDestinationList::SetUsageData(
    _In_ IUnknown *punk,
    _In_opt_ FLOAT *pflPoints,
    _In_opt_ FILETIME *pftLastAccess)
{
    CAutoDestList List;
    int iEntry;
    HRESULT hr;

    if (FAILED(m_hrInit))
        return E_UNEXPECTED;

    _ReleaseList();
    hr = _EnsureItemGetIndex(punk, &List, FALSE, &iEntry, NULL);
    if (SUCCEEDED(hr))
        hr = List.SetData(iEntry, pflPoints, pftLastAccess);
    if (SUCCEEDED(hr))
        hr = List.Persist();
    if (SUCCEEDED(hr))
        JumpListCounterIncrement();
    return hr;
}

HRESULT
STDMETHODCALLTYPE
CAutomaticDestinationList::GetUsageData(
    _In_ IUnknown *punk,
    _Out_ FLOAT *pflPoints,
    _Out_ FILETIME *pftLastAccess)
{
    CAutoDestList List;
    int iEntry;
    HRESULT hr;

    if (FAILED(m_hrInit))
        return E_UNEXPECTED;

    _ReleaseList();
    hr = _GetIndex(punk, &List, &iEntry);
    if (SUCCEEDED(hr))
    {
        PDESTLIST_ENTRY pEntry = List.GetEntry(iEntry);

        *pflPoints = pEntry->Data.flPoints;
        *pftLastAccess = pEntry->Data.ftLastAccess;
    }
    return hr;
}

HRESULT
STDMETHODCALLTYPE
CAutomaticDestinationList::ResolveDestination(
    _In_opt_ HWND hwnd,
    _In_ DWORD dwFlags,
    _In_ IShellItem *psi,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    CComPtr<IUnknown> punkResolved;
    CAutoDestList List;
    int iEntry;
    HRESULT hr;

    *ppv = NULL;
    if (FAILED(m_hrInit))
        return E_UNEXPECTED;

    _ReleaseList();
    hr = _GetIndex(psi, &List, &iEntry);
    if (SUCCEEDED(hr))
        hr = List.ResolveDestination(hwnd, dwFlags, iEntry, IID_PPV_ARG(IUnknown, &punkResolved));
    if (SUCCEEDED(hr))
        hr = List.Persist();
    if (SUCCEEDED(hr))
        hr = punkResolved->QueryInterface(riid, ppv);
    return hr;
}

HRESULT
STDMETHODCALLTYPE
CAutomaticDestinationList::ClearList(
    _In_ BOOL fClearPinned)
{
    HRESULT hr;

    if (FAILED(m_hrInit))
        return E_UNEXPECTED;

    hr = _EnsureDirectory();
    if (FAILED(hr))
        return hr;

    _ReleaseList();
    return s_DeleteList(m_szListPath, m_pszAppID, m_pszAppPath, fClearPinned ? 3 : 1);
}

/* uFlags 1 drops the unpinned entries, 2 the pinned ones */
HRESULT
CAutomaticDestinationList::s_DeleteList(
    _In_ PCWSTR pszListPath,
    _In_opt_ PCWSTR pszAppID,
    _In_opt_ PCWSTR pszAppPath,
    _In_ UINT uFlags)
{
    CAutoDestList List;
    HRESULT hr = S_OK;

    List.SetOwner(pszListPath, pszAppID, pszAppPath);
    switch (uFlags & 3)
    {
        case 3:
            List.ClearAll();
            break;

        case 1:
        case 2:
            hr = List.EnsureLoaded();
            if (FAILED(hr))
                break;

            List.m_fDirty = TRUE;
            for (int i = (int)List.m_Header.cEntries - 1; i >= 0; i -= 1)
            {
                BOOL fPinned = List.GetEntry(i)->Data.iPinPos != DESTLIST_NOT_PINNED;

                if ((fPinned && (uFlags & 2)) || (!fPinned && (uFlags & 1)))
                    hr = List.RemoveMatchingEntry(i);
            }
            break;
    }

    if (SUCCEEDED(hr))
    {
        hr = List.Persist();
        if (SUCCEEDED(hr))
            JumpListCounterIncrement();
    }
    return hr;
}
