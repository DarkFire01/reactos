/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Property stores and property lists of shell items
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

HRESULT
SHELL_CreateItemPropertyStore(
    _In_ PCIDLIST_ABSOLUTE pidl,
    _In_ GETPROPERTYSTOREFLAGS flags,
    _In_reads_opt_(cKeys) const PROPERTYKEY *rgKeys,
    _In_ UINT cKeys,
    _In_ REFIID riid,
    _Outptr_ void **ppv);

HRESULT
SHELL_CreateItemArrayPropertyStore(
    _In_ IShellItemArray *psia,
    _In_ GETPROPERTYSTOREFLAGS flags,
    _In_ REFIID riid,
    _Outptr_ void **ppv);

HRESULT
SHELL_CreateItemArrayPropertyDescriptionList(
    _In_ IShellItemArray *psia,
    _In_ REFPROPERTYKEY keyType,
    _In_ REFIID riid,
    _Outptr_ void **ppv);
