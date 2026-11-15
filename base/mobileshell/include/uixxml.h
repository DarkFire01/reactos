/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     Small XML tree loader over XmlLite, shared by markup, themes and config
 */

#pragma once

#include <windows.h>
#include <shlwapi.h>
#include <xmllite.h>
#include <string>
#include <vector>
#include <utility>
#include "uixptr.h"

typedef struct _UIX_XML_NODE
{
    std::wstring Name;
    std::vector<std::pair<std::wstring, std::wstring>> Attributes;
    std::vector<struct _UIX_XML_NODE> Children;

    PCWSTR
    Attr(
        _In_z_ PCWSTR Key) const
    {
        for (const auto &Pair : Attributes)
        {
            if (_wcsicmp(Pair.first.c_str(), Key) == 0)
                return Pair.second.c_str();
        }
        return NULL;
    }

    PCWSTR
    Attr(
        _In_z_ PCWSTR Key,
        _In_z_ PCWSTR Default) const
    {
        PCWSTR Value = Attr(Key);

        return Value ? Value : Default;
    }
} UIX_XML_NODE, *PUIX_XML_NODE;

inline
HRESULT
UixXmlLoad(
    _In_z_ PCWSTR Path,
    _Out_ PUIX_XML_NODE Root)
{
    UixPtr<IStream> Stream;
    UixPtr<IXmlReader> Reader;
    std::vector<PUIX_XML_NODE> Stack;
    XmlNodeType Type;
    PCWSTR Name;
    PCWSTR Value;
    BOOL Empty;
    BOOL HaveRoot = FALSE;
    HRESULT Status;

    Root->Name.clear();
    Root->Attributes.clear();
    Root->Children.clear();

    Status = SHCreateStreamOnFileEx(Path, STGM_READ | STGM_SHARE_DENY_WRITE, FILE_ATTRIBUTE_NORMAL, FALSE, NULL, Stream.Put());
    if (FAILED(Status))
        return Status;

    Status = CreateXmlReader(__uuidof(IXmlReader), Reader.PutVoid(), NULL);
    if (FAILED(Status))
        return Status;

    Reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit);
    Status = Reader->SetInput(Stream.Get());
    if (FAILED(Status))
        return Status;

    while ((Status = Reader->Read(&Type)) == S_OK)
    {
        if (Type == XmlNodeType_Element)
        {
            PUIX_XML_NODE Node;

            if (!HaveRoot)
            {
                Node = Root;
                HaveRoot = TRUE;
            }
            else if (!Stack.empty())
            {
                Stack.back()->Children.emplace_back();
                Node = &Stack.back()->Children.back();
            }
            else
            {
                /* Second top level element, not valid XML */
                return E_FAIL;
            }

            Reader->GetLocalName(&Name, NULL);
            Node->Name = Name;
            Empty = Reader->IsEmptyElement();

            if (Reader->MoveToFirstAttribute() == S_OK)
            {
                do
                {
                    Reader->GetLocalName(&Name, NULL);
                    Reader->GetValue(&Value, NULL);
                    Node->Attributes.emplace_back(Name, Value);
                } while (Reader->MoveToNextAttribute() == S_OK);
                Reader->MoveToElement();
            }

            /* Only open elements are stacked, a reallocation only moves closed siblings */
            if (!Empty)
                Stack.push_back(Node);
        }
        else if (Type == XmlNodeType_EndElement)
        {
            if (!Stack.empty())
                Stack.pop_back();
        }
    }

    return (FAILED(Status) || !HaveRoot) ? (FAILED(Status) ? Status : E_FAIL) : S_OK;
}
