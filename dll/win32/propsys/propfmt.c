/*
 * PROJECT:     ReactOS Property System
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Display text of property values
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"
#include <winuser.h>
#include <winreg.h>
#include <propvarutil.h>
#include "propdesc.h"

#include <strsafe.h>

WINE_DEFAULT_DEBUG_CHANNEL(propsys);

#define PROPFMT_CCH     260

/* Attribute letters in the order Windows lists them */
static const struct
{
    DWORD dwAttribute;
    WCHAR chLetter;
} c_rgAttributeLetters[] =
{
    { FILE_ATTRIBUTE_READONLY, L'R' },
    { FILE_ATTRIBUTE_HIDDEN, L'H' },
    { FILE_ATTRIBUTE_SYSTEM, L'S' },
    { FILE_ATTRIBUTE_DIRECTORY, L'D' },
    { FILE_ATTRIBUTE_ARCHIVE, L'A' },
    { FILE_ATTRIBUTE_DEVICE, L'X' },
    { FILE_ATTRIBUTE_NORMAL, L'N' },
    { FILE_ATTRIBUTE_TEMPORARY, L'T' },
    { FILE_ATTRIBUTE_REPARSE_POINT, L'L' },
    { FILE_ATTRIBUTE_COMPRESSED, L'C' },
    { FILE_ATTRIBUTE_OFFLINE, L'O' },
    { FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, L'I' },
    { FILE_ATTRIBUTE_ENCRYPTED, L'E' },
    { 0x00010000, L'V' },
};

static
HRESULT
PropFormat_Dup(
    _In_ PCWSTR psz,
    _Outptr_ PWSTR *ppsz)
{
    SIZE_T cb = (lstrlenW(psz) + 1) * sizeof(WCHAR);

    *ppsz = CoTaskMemAlloc(cb);
    if (!*ppsz)
        return E_OUTOFMEMORY;
    CopyMemory(*ppsz, psz, cb);
    return S_OK;
}

/* Puts a number string through the user's number format, with the given decimals and grouping */
static
VOID
PropFormat_Number(
    _In_ PCWSTR pszNumber,
    _In_ UINT cDecimals,
    _In_ BOOL fGrouping,
    _Out_writes_(cch) PWSTR psz,
    _In_ UINT cch)
{
    WCHAR szDecimal[8];
    WCHAR szThousand[8];
    WCHAR szGrouping[16];
    NUMBERFMTW nf;

    GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SDECIMAL, szDecimal, _countof(szDecimal));
    GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_STHOUSAND, szThousand, _countof(szThousand));
    GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SGROUPING, szGrouping, _countof(szGrouping));

    nf.NumDigits = cDecimals;
    nf.LeadingZero = 1;
    nf.Grouping = (fGrouping && szGrouping[0] >= L'0' && szGrouping[0] <= L'9') ? szGrouping[0] - L'0' : 0;
    nf.lpDecimalSep = szDecimal;
    nf.lpThousandSep = szThousand;
    nf.NegativeOrder = 1;

    if (!GetNumberFormatW(LOCALE_USER_DEFAULT, 0, pszNumber, &nf, psz, cch))
        StringCchCopyW(psz, cch, pszNumber);
}

/* Fills a resource template that has one %s */
static
HRESULT
PropFormat_WithUnit(
    _In_ UINT idsTemplate,
    _In_ PCWSTR pszNumber,
    _Outptr_ PWSTR *ppszDisplay)
{
    WCHAR szTemplate[64];
    WCHAR szText[PROPFMT_CCH];

    if (!LoadStringW(PROPSYS_HINST, idsTemplate, szTemplate, _countof(szTemplate)))
        StringCchCopyW(szTemplate, _countof(szTemplate), L"%s");
    StringCchPrintfW(szText, _countof(szText), szTemplate, pszNumber);
    return PropFormat_Dup(szText, ppszDisplay);
}

/* Byte counts with three significant digits, cut rather than rounded, as Windows shows them */
static
HRESULT
PropFormat_FileSize(
    _In_ ULONGLONG cb,
    _In_ PROPDESC_FORMAT_FLAGS pdff,
    _Outptr_ PWSTR *ppszDisplay)
{
    static const UINT c_rgidsUnits[] =
    {
        IDS_SIZE_KB, IDS_SIZE_MB, IDS_SIZE_GB, IDS_SIZE_TB, IDS_SIZE_PB, IDS_SIZE_EB
    };
    WCHAR szRaw[64];
    WCHAR szNumber[64];
    ULONGLONG ullUnit = 1024;
    ULONGLONG ullScaled;
    UINT iUnit = 0;
    UINT cDecimals;

    if (pdff & PDFF_ALWAYSKB)
    {
        StringCchPrintfW(szRaw, _countof(szRaw), L"%I64u", (cb + 1023) / 1024);
        PropFormat_Number(szRaw, 0, TRUE, szNumber, _countof(szNumber));
        return PropFormat_WithUnit(IDS_SIZE_KB, szNumber, ppszDisplay);
    }

    if (cb < 1024)
    {
        StringCchPrintfW(szRaw, _countof(szRaw), L"%I64u", cb);
        return PropFormat_WithUnit(IDS_SIZE_BYTES, szRaw, ppszDisplay);
    }

    while (iUnit + 1 < _countof(c_rgidsUnits) && cb / ullUnit >= 1000)
    {
        ullUnit *= 1024;
        iUnit++;
    }

    /* Hundredths of the unit, then drop digits past the third significant one */
    ullScaled = (ULONGLONG)(((double)cb / (double)ullUnit) * 100.0);
    if (ullScaled < 1000)
        cDecimals = 2;
    else if (ullScaled < 10000)
        cDecimals = 1;
    else
        cDecimals = 0;

    if (cDecimals == 2)
        StringCchPrintfW(szRaw, _countof(szRaw), L"%I64u.%02I64u", ullScaled / 100, ullScaled % 100);
    else if (cDecimals == 1)
        StringCchPrintfW(szRaw, _countof(szRaw), L"%I64u.%I64u", ullScaled / 100, (ullScaled / 10) % 10);
    else
        StringCchPrintfW(szRaw, _countof(szRaw), L"%I64u", ullScaled / 100);

    PropFormat_Number(szRaw, cDecimals, FALSE, szNumber, _countof(szNumber));
    return PropFormat_WithUnit(c_rgidsUnits[iUnit], szNumber, ppszDisplay);
}

static
BOOL
PropFormat_GetFileTime(
    _In_ REFPROPVARIANT propvar,
    _Out_ SYSTEMTIME *pst)
{
    FILETIME ftLocal;

    if (propvar->vt == VT_FILETIME)
    {
        return FileTimeToLocalFileTime(&propvar->filetime, &ftLocal) &&
               FileTimeToSystemTime(&ftLocal, pst);
    }
    if (propvar->vt == VT_DATE)
        return VariantTimeToSystemTime(propvar->date, pst);
    return FALSE;
}

static
VOID
PropFormat_DateTimeText(
    _In_ const SYSTEMTIME *pst,
    _In_ PROPDESC_FORMAT_FLAGS pdff,
    _Out_writes_(cch) PWSTR psz,
    _In_ UINT cch)
{
    WCHAR szDate[128] = L"";
    WCHAR szTime[64] = L"";

    if (!(pdff & PDFF_HIDEDATE))
    {
        GetDateFormatW(LOCALE_USER_DEFAULT, (pdff & PDFF_LONGDATE) ? DATE_LONGDATE : DATE_SHORTDATE,
                       pst, NULL, szDate, _countof(szDate));
    }
    if (!(pdff & PDFF_HIDETIME))
    {
        GetTimeFormatW(LOCALE_USER_DEFAULT, (pdff & PDFF_LONGTIME) ? 0 : TIME_NOSECONDS,
                       pst, NULL, szTime, _countof(szTime));
    }

    if (szDate[0] && szTime[0])
        StringCchPrintfW(psz, cch, (pdff & PDFF_LONGDATE) ? L"%s, %s" : L"%s %s", szDate, szTime);
    else
        StringCchCopyW(psz, cch, szDate[0] ? szDate : szTime);
}

static
HRESULT
PropFormat_DateTime(
    _In_ REFPROPVARIANT propvar,
    _In_ PROPDESC_FORMAT_FLAGS pdff,
    _Outptr_ PWSTR *ppszDisplay)
{
    WCHAR szText[PROPFMT_CCH];
    SYSTEMTIME st;

    if (!PropFormat_GetFileTime(propvar, &st))
        return PropVariantToStringAlloc(propvar, ppszDisplay);

    PropFormat_DateTimeText(&st, pdff, szText, _countof(szText));
    return PropFormat_Dup(szText, ppszDisplay);
}

/* A two element date vector is a range, the aggregate of several items' dates */
static
HRESULT
PropFormat_DateRange(
    _In_ REFPROPVARIANT propvar,
    _In_ PROPDESC_FORMAT_FLAGS pdff,
    _Outptr_ PWSTR *ppszDisplay)
{
    WCHAR szFirst[PROPFMT_CCH];
    WCHAR szLast[PROPFMT_CCH];
    WCHAR szTemplate[32];
    FILETIME ftLocal;
    SYSTEMTIME st;
    DWORD_PTR rgArgs[2];
    PWSTR pszOut = NULL;
    HRESULT hr;

    if (!FileTimeToLocalFileTime(&propvar->cafiletime.pElems[0], &ftLocal) ||
        !FileTimeToSystemTime(&ftLocal, &st))
    {
        return E_INVALIDARG;
    }
    PropFormat_DateTimeText(&st, pdff, szFirst, _countof(szFirst));

    if (!FileTimeToLocalFileTime(&propvar->cafiletime.pElems[1], &ftLocal) ||
        !FileTimeToSystemTime(&ftLocal, &st))
    {
        return E_INVALIDARG;
    }
    PropFormat_DateTimeText(&st, pdff, szLast, _countof(szLast));

    if (!lstrcmpW(szFirst, szLast))
        return PropFormat_Dup(szFirst, ppszDisplay);

    if (!LoadStringW(PROPSYS_HINST, IDS_DATE_RANGE, szTemplate, _countof(szTemplate)))
        StringCchCopyW(szTemplate, _countof(szTemplate), L"%1 - %2");

    rgArgs[0] = (DWORD_PTR)szFirst;
    rgArgs[1] = (DWORD_PTR)szLast;
    if (!FormatMessageW(FORMAT_MESSAGE_FROM_STRING | FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_ARGUMENT_ARRAY,
                        szTemplate, 0, 0, (LPWSTR)&pszOut, 0, (va_list *)rgArgs))
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    hr = PropFormat_Dup(pszOut, ppszDisplay);
    LocalFree(pszOut);
    return hr;
}

/* "30.0 GB free of 60.0 GB" from the free and total byte counts */
static
HRESULT
PropFormat_FreeSpace(
    _In_ REFPROPVARIANT propvar,
    _In_ PROPDESC_FORMAT_FLAGS pdff,
    _Outptr_ PWSTR *ppszDisplay)
{
    WCHAR szTemplate[64];
    PWSTR pszFree = NULL;
    PWSTR pszTotal = NULL;
    PWSTR pszOut = NULL;
    DWORD_PTR rgArgs[2];
    HRESULT hr;

    if (propvar->vt != (VT_VECTOR | VT_UI8) || propvar->cauh.cElems < 2)
        return E_INVALIDARG;

    hr = PropFormat_FileSize(propvar->cauh.pElems[0].QuadPart, pdff, &pszFree);
    if (SUCCEEDED(hr))
        hr = PropFormat_FileSize(propvar->cauh.pElems[1].QuadPart, pdff, &pszTotal);
    if (SUCCEEDED(hr))
    {
        if (!LoadStringW(PROPSYS_HINST, IDS_FREESPACE, szTemplate, _countof(szTemplate)))
            StringCchCopyW(szTemplate, _countof(szTemplate), L"%1 free of %2");

        rgArgs[0] = (DWORD_PTR)pszFree;
        rgArgs[1] = (DWORD_PTR)pszTotal;
        if (FormatMessageW(FORMAT_MESSAGE_FROM_STRING | FORMAT_MESSAGE_ALLOCATE_BUFFER |
                           FORMAT_MESSAGE_ARGUMENT_ARRAY,
                           szTemplate, 0, 0, (LPWSTR)&pszOut, 0, (va_list *)rgArgs))
        {
            hr = PropFormat_Dup(pszOut, ppszDisplay);
            LocalFree(pszOut);
        }
        else
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
        }
    }
    CoTaskMemFree(pszFree);
    CoTaskMemFree(pszTotal);
    return hr;
}

static
HRESULT
PropFormat_Attributes(
    _In_ ULONG ulAttributes,
    _Outptr_ PWSTR *ppszDisplay)
{
    WCHAR szText[_countof(c_rgAttributeLetters) + 1];
    UINT cch = 0;
    UINT i;

    for (i = 0; i < _countof(c_rgAttributeLetters); i++)
    {
        if (ulAttributes & c_rgAttributeLetters[i].dwAttribute)
            szText[cch++] = c_rgAttributeLetters[i].chLetter;
    }
    szText[cch] = UNICODE_NULL;
    return PropFormat_Dup(szText, ppszDisplay);
}

static
HRESULT
PropFormat_Enumerated(
    _In_ const PROPSCHEMA_ENTRY *pEntry,
    _In_ REFPROPVARIANT propvar,
    _Outptr_ PWSTR *ppszDisplay)
{
    IPropertyDescription *ppd;
    IPropertyEnumTypeList *pList;
    IPropertyEnumType *pEnum;
    HRESULT hr;
    UINT i;

    hr = PropDesc_Create(pEntry, 0, &IID_IPropertyDescription, (void **)&ppd);
    if (FAILED(hr))
        return hr;

    hr = IPropertyDescription_GetEnumTypeList(ppd, &IID_IPropertyEnumTypeList, (void **)&pList);
    IPropertyDescription_Release(ppd);
    if (FAILED(hr))
        return hr;

    hr = IPropertyEnumTypeList_FindMatchingIndex(pList, propvar, &i);
    if (SUCCEEDED(hr))
        hr = IPropertyEnumTypeList_GetAt(pList, i, &IID_IPropertyEnumType, (void **)&pEnum);
    if (SUCCEEDED(hr))
    {
        hr = IPropertyEnumType_GetDisplayText(pEnum, ppszDisplay);
        IPropertyEnumType_Release(pEnum);
    }
    IPropertyEnumTypeList_Release(pList);
    return hr;
}

static
HRESULT
PropFormat_Plain(
    _In_ REFPROPVARIANT propvar,
    _Outptr_ PWSTR *ppszDisplay)
{
    WCHAR szText[64];

    switch (propvar->vt)
    {
        case VT_BOOL:
            return PropSchema_LoadString(propvar->boolVal ? IDS_YES : IDS_NO, ppszDisplay);

        case VT_R4:
            StringCchPrintfW(szText, _countof(szText), L"%g", (double)propvar->fltVal);
            return PropFormat_Dup(szText, ppszDisplay);

        case VT_R8:
            StringCchPrintfW(szText, _countof(szText), L"%g", propvar->dblVal);
            return PropFormat_Dup(szText, ppszDisplay);

        case VT_I1:
        case VT_I2:
        case VT_I4:
        case VT_I8:
        case VT_INT:
        {
            LONGLONG llValue;

            if (FAILED(PropVariantToInt64(propvar, &llValue)))
                break;
            StringCchPrintfW(szText, _countof(szText), L"%I64d", llValue);
            return PropFormat_Dup(szText, ppszDisplay);
        }

        case VT_UI1:
        case VT_UI2:
        case VT_UI4:
        case VT_UI8:
        case VT_UINT:
        {
            ULONGLONG ullValue;

            if (FAILED(PropVariantToUInt64(propvar, &ullValue)))
                break;
            StringCchPrintfW(szText, _countof(szText), L"%I64u", ullValue);
            return PropFormat_Dup(szText, ppszDisplay);
        }

        default:
            break;
    }

    if (propvar->vt == VT_LPWSTR || propvar->vt == VT_BSTR)
        return PropVariantToStringAlloc(propvar, ppszDisplay);

    {
        PROPVARIANT pvText;
        HRESULT hr = PropVariantChangeType(&pvText, propvar, 0, VT_LPWSTR);

        if (FAILED(hr))
            return hr;
        *ppszDisplay = pvText.pwszVal;
        return S_OK;
    }
}

static
HRESULT
PropFormat_Scalar(
    _In_ const PROPSCHEMA_ENTRY *pEntry,
    _In_ REFPROPVARIANT propvar,
    _In_ PROPDESC_FORMAT_FLAGS pdff,
    _Outptr_ PWSTR *ppszDisplay)
{
    WCHAR szRaw[64];
    ULONGLONG ullValue = 0;
    ULONG ulValue;
    double dblValue;

    switch (pEntry->format)
    {
        case PSFMT_FILESIZE:
            if (SUCCEEDED(PropVariantToUInt64(propvar, &ullValue)))
                return PropFormat_FileSize(ullValue, pdff, ppszDisplay);
            break;

        case PSFMT_DURATION:
            if (SUCCEEDED(PropVariantToUInt64(propvar, &ullValue)))
            {
                ULONGLONG ullSeconds = ullValue / 10000000;

                StringCchPrintfW(szRaw, _countof(szRaw), L"%02I64u:%02u:%02u", ullSeconds / 3600,
                                 (UINT)((ullSeconds / 60) % 60), (UINT)(ullSeconds % 60));
                return PropFormat_Dup(szRaw, ppszDisplay);
            }
            break;

        case PSFMT_KBPS:
            if (SUCCEEDED(PropVariantToUInt32(propvar, &ulValue)))
            {
                StringCchPrintfW(szRaw, _countof(szRaw), L"%u", ulValue / 1000);
                return PropFormat_WithUnit(IDS_UNIT_KBPS, szRaw, ppszDisplay);
            }
            break;

        case PSFMT_KHZ:
            if (SUCCEEDED(PropVariantToUInt32(propvar, &ulValue)))
            {
                WCHAR szNumber[64];

                StringCchPrintfW(szRaw, _countof(szRaw), L"%u.%03u", ulValue / 1000, ulValue % 1000);
                PropFormat_Number(szRaw, 3, FALSE, szNumber, _countof(szNumber));
                return PropFormat_WithUnit(IDS_UNIT_KHZ, szNumber, ppszDisplay);
            }
            break;

        case PSFMT_FRAMERATE:
            if (SUCCEEDED(PropVariantToUInt32(propvar, &ulValue)))
            {
                WCHAR szNumber[64];

                StringCchPrintfW(szRaw, _countof(szRaw), L"%u.%02u", ulValue / 1000, (ulValue % 1000) / 10);
                PropFormat_Number(szRaw, 2, FALSE, szNumber, _countof(szNumber));
                return PropFormat_WithUnit(IDS_UNIT_FRAMERATE, szNumber, ppszDisplay);
            }
            break;

        case PSFMT_DPI:
            if (SUCCEEDED(PropVariantToDouble(propvar, &dblValue)))
            {
                StringCchPrintfW(szRaw, _countof(szRaw), L"%g", dblValue);
                return PropFormat_WithUnit(IDS_UNIT_DPI, szRaw, ppszDisplay);
            }
            break;

        case PSFMT_PIXELS:
            if (SUCCEEDED(PropVariantToUInt32(propvar, &ulValue)))
            {
                StringCchPrintfW(szRaw, _countof(szRaw), L"%u", ulValue);
                return PropFormat_WithUnit(IDS_UNIT_PIXELS, szRaw, ppszDisplay);
            }
            break;

        case PSFMT_PERCENT:
            if (SUCCEEDED(PropVariantToUInt32(propvar, &ulValue)))
            {
                StringCchPrintfW(szRaw, _countof(szRaw), L"%u", ulValue);
                return PropFormat_WithUnit(IDS_UNIT_PERCENT, szRaw, ppszDisplay);
            }
            break;

        case PSFMT_ATTRIBUTES:
            if (SUCCEEDED(PropVariantToUInt32(propvar, &ulValue)))
                return PropFormat_Attributes(ulValue, ppszDisplay);
            break;

        default:
            break;
    }

    switch (pEntry->displayType)
    {
        case PDDT_DATETIME:
            return PropFormat_DateTime(propvar, pdff, ppszDisplay);

        case PDDT_ENUMERATED:
            if (SUCCEEDED(PropFormat_Enumerated(pEntry, propvar, ppszDisplay)))
                return S_OK;
            break;

        case PDDT_BOOLEAN:
        {
            BOOL fValue;

            if (SUCCEEDED(PropVariantToBoolean(propvar, &fValue)))
                return PropSchema_LoadString(fValue ? IDS_YES : IDS_NO, ppszDisplay);
            break;
        }

        default:
            break;
    }
    return PropFormat_Plain(propvar, ppszDisplay);
}

/* Copies one element of a vector value into a scalar value that shares nothing with it */
static
HRESULT
PropFormat_GetElement(
    _In_ REFPROPVARIANT propvar,
    _In_ ULONG iElem,
    _Out_ PROPVARIANT *ppropvar)
{
    PropVariantInit(ppropvar);
    switch (propvar->vt & ~VT_VECTOR)
    {
        case VT_LPWSTR:
            ppropvar->vt = VT_LPWSTR;
            return PropFormat_Dup(propvar->calpwstr.pElems[iElem] ? propvar->calpwstr.pElems[iElem] : L"",
                                  &ppropvar->pwszVal);
        case VT_BSTR:
            ppropvar->vt = VT_LPWSTR;
            return PropFormat_Dup(propvar->cabstr.pElems[iElem] ? propvar->cabstr.pElems[iElem] : L"",
                                  &ppropvar->pwszVal);
        case VT_I2:
            ppropvar->vt = VT_I2;
            ppropvar->iVal = propvar->cai.pElems[iElem];
            return S_OK;
        case VT_UI2:
            ppropvar->vt = VT_UI2;
            ppropvar->uiVal = propvar->caui.pElems[iElem];
            return S_OK;
        case VT_I4:
            ppropvar->vt = VT_I4;
            ppropvar->lVal = propvar->cal.pElems[iElem];
            return S_OK;
        case VT_UI4:
            ppropvar->vt = VT_UI4;
            ppropvar->ulVal = propvar->caul.pElems[iElem];
            return S_OK;
        case VT_I8:
            ppropvar->vt = VT_I8;
            ppropvar->hVal = propvar->cah.pElems[iElem];
            return S_OK;
        case VT_UI8:
            ppropvar->vt = VT_UI8;
            ppropvar->uhVal = propvar->cauh.pElems[iElem];
            return S_OK;
        case VT_R8:
            ppropvar->vt = VT_R8;
            ppropvar->dblVal = propvar->cadbl.pElems[iElem];
            return S_OK;
        case VT_BOOL:
            ppropvar->vt = VT_BOOL;
            ppropvar->boolVal = propvar->cabool.pElems[iElem];
            return S_OK;
        case VT_FILETIME:
            ppropvar->vt = VT_FILETIME;
            ppropvar->filetime = propvar->cafiletime.pElems[iElem];
            return S_OK;
        default:
            return E_NOTIMPL;
    }
}

/* Multiple values are listed with "; " between them */
static
HRESULT
PropFormat_Vector(
    _In_ const PROPSCHEMA_ENTRY *pEntry,
    _In_ REFPROPVARIANT propvar,
    _In_ PROPDESC_FORMAT_FLAGS pdff,
    _Outptr_ PWSTR *ppszDisplay)
{
    PWSTR pszResult = NULL;
    SIZE_T cchResult = 0;
    ULONG cElems = propvar->cal.cElems;
    ULONG i;
    HRESULT hr = S_OK;

    for (i = 0; i < cElems && SUCCEEDED(hr); i++)
    {
        PROPVARIANT pvElem;
        PWSTR pszElem = NULL;

        hr = PropFormat_GetElement(propvar, i, &pvElem);
        if (FAILED(hr))
            break;
        hr = PropFormat_Scalar(pEntry, &pvElem, pdff, &pszElem);
        PropVariantClear(&pvElem);
        if (FAILED(hr))
            break;

        if (pszElem[0])
        {
            SIZE_T cchElem = lstrlenW(pszElem);
            SIZE_T cchNew = cchResult + (cchResult ? 2 : 0) + cchElem + 1;
            PWSTR pszNew = CoTaskMemRealloc(pszResult, cchNew * sizeof(WCHAR));

            if (!pszNew)
            {
                hr = E_OUTOFMEMORY;
            }
            else
            {
                if (cchResult)
                {
                    pszNew[cchResult++] = L';';
                    pszNew[cchResult++] = L' ';
                }
                CopyMemory(pszNew + cchResult, pszElem, (cchElem + 1) * sizeof(WCHAR));
                cchResult += cchElem;
                pszResult = pszNew;
            }
        }
        CoTaskMemFree(pszElem);
    }

    if (FAILED(hr))
    {
        CoTaskMemFree(pszResult);
        return hr;
    }
    if (!pszResult)
        return PropFormat_Dup(L"", ppszDisplay);
    *ppszDisplay = pszResult;
    return S_OK;
}

static
BOOL
PropFormat_IsEmpty(
    _In_ REFPROPVARIANT propvar)
{
    if (propvar->vt == VT_EMPTY || propvar->vt == VT_NULL)
        return TRUE;
    if ((propvar->vt & VT_VECTOR) && !propvar->cal.cElems)
        return TRUE;
    if (propvar->vt == VT_LPWSTR && (!propvar->pwszVal || !propvar->pwszVal[0]))
        return TRUE;
    if (propvar->vt == VT_BSTR && (!propvar->bstrVal || !propvar->bstrVal[0]))
        return TRUE;
    return FALSE;
}

/* Empty values give the edit invitation when asked for, the enumeration default, or empty text */
HRESULT
PropFormat_Value(
    _In_ const PROPSCHEMA_ENTRY *pEntry,
    _In_ REFPROPVARIANT propvar,
    _In_ PROPDESC_FORMAT_FLAGS pdff,
    _Outptr_ PWSTR *ppszDisplay)
{
    HRESULT hr;

    if (!ppszDisplay)
        return E_POINTER;
    *ppszDisplay = NULL;
    if (!propvar)
        return E_INVALIDARG;

    if (PropFormat_IsEmpty(propvar))
    {
        if ((pdff & PDFF_USEEDITINVITATION) && pEntry->idsEditInvitation)
            return PropSchema_LoadString(pEntry->idsEditInvitation, ppszDisplay);

        if (pEntry->displayType == PDDT_ENUMERATED && SUCCEEDED(PropFormat_Enumerated(pEntry, propvar, ppszDisplay)))
            return S_OK;

        hr = PropFormat_Dup(L"", ppszDisplay);
        return SUCCEEDED(hr) ? S_FALSE : hr;
    }

    if (propvar->vt & VT_VECTOR)
    {
        if (pEntry->format == PSFMT_FREESPACE)
            return PropFormat_FreeSpace(propvar, pdff, ppszDisplay);
        if (propvar->vt == (VT_VECTOR | VT_FILETIME) && propvar->cafiletime.cElems == 2)
            return PropFormat_DateRange(propvar, pdff, ppszDisplay);
        return PropFormat_Vector(pEntry, propvar, pdff, ppszDisplay);
    }
    return PropFormat_Scalar(pEntry, propvar, pdff, ppszDisplay);
}
