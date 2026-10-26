/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Label element of the common item dialog custom controls
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"
#include "cidlabel.h"

/* Width and height that leave a label's text unconstrained */
#define CIDLABEL_UNCONSTRAINED 0x7FFF

DirectUI::IClassInfo *CCIDLabel::Class = NULL;

/* Labels react to the mouse so their tooltip can show */
HRESULT
WINAPI
CCIDLabel::Create(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    HRESULT hr;

    hr = DirectUI::CreateAndInit<CCIDLabel, UINT>(0, peParent, pdwDeferCookie, ppElement);
    if (SUCCEEDED(hr))
        (*ppElement)->SetActive(1);
    return hr;
}

HRESULT
WINAPI
CCIDLabel::Register()
{
    return DirectUI::ClassInfo<CCIDLabel, DirectUI::Element,
                               DirectUI::StandardCreator<CCIDLabel>>::Register(
        DUIFRAME_HINST, L"CIDLabel", NULL, 0, false);
}

/*
 * The accessible description is the content without its mnemonic. A width narrower than
 * the text needs turns the tooltip on.
 */
void
CCIDLabel::OnPropertyChanged(
    _In_ const DirectUI::PropertyInfo *ppi,
    _In_ int iIndex,
    _In_opt_ DirectUI::Value *pvOld,
    _In_opt_ DirectUI::Value *pvNew)
{
    BOOL fContent;
    HDC hdc;

    DirectUI::Element::OnPropertyChanged(ppi, iIndex, pvOld, pvNew);

    fContent = (ppi == ContentProp() && (int)(ppi->Flags & 3) == iIndex);
    if (!fContent && !(ppi == WidthProp() && (int)(ppi->Flags & 3) == iIndex))
        return;

    if (fContent)
    {
        DirectUI::Value *pv = GetValue(ContentProp, 2, NULL);

        if (pv->GetType() == (int)DirectUI::ValueType::String)
        {
            PWSTR pszDesc;

            if (SUCCEEDED(SHStrDupW(pv->GetString(), &pszDesc)))
            {
                SHStripMneumonicW(pszDesc);
                SetAccDesc(pszDesc);
                CoTaskMemFree(pszDesc);
            }
        }
        pv->Release();
    }

    hdc = GetDC(NULL);
    if (hdc)
    {
        DirectUI::DCSurface srf(hdc);
        SIZE size = GetContentSize(CIDLABEL_UNCONSTRAINED, CIDLABEL_UNCONSTRAINED, &srf);

        SetTooltip(GetWidth() < size.cx);
        ReleaseDC(NULL, hdc);
    }
}
