/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Label element of the common item dialog custom controls
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* A label that shows its full text as a tooltip once it is cut off */
class CCIDLabel : public DirectUI::Element
{
public:
    static HRESULT WINAPI Create(
        _In_opt_ DirectUI::Element *peParent,
        _Inout_opt_ ULONG *pdwDeferCookie,
        _Outptr_ DirectUI::Element **ppElement);
    static HRESULT WINAPI Register();
    static DirectUI::IClassInfo *WINAPI GetClassInfoPtr() { return Class; }
    virtual DirectUI::IClassInfo *GetClassInfoW() { return Class; }

    virtual void OnPropertyChanged(
        _In_ const DirectUI::PropertyInfo *ppi,
        _In_ int iIndex,
        _In_opt_ DirectUI::Value *pvOld,
        _In_opt_ DirectUI::Value *pvNew);

    static DirectUI::IClassInfo *Class;
};
