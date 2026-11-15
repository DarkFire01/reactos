/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     Tab separated record files under the user's local app data
 */

#pragma once

#include <windows.h>
#include <shlobj.h>
#include <string>
#include <vector>

typedef std::vector<std::wstring> APP_RECORD;

inline
std::wstring
AppDataFile(
    _In_z_ PCWSTR Name)
{
    PWSTR Base = NULL;
    std::wstring Path;

    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &Base)))
    {
        Path = Base;
        Path += L"\\ReactOSMobile";
        CreateDirectoryW(Path.c_str(), NULL);
        Path += L"\\";
        Path += Name;
    }
    CoTaskMemFree(Base);
    return Path;
}

/* Returns FALSE when the file does not exist yet, so callers can seed it */
inline
BOOL
LoadRecords(
    _In_z_ PCWSTR Name,
    _Out_ std::vector<APP_RECORD> &Records)
{
    std::wstring Path = AppDataFile(Name);
    std::wstring Text;
    HANDLE File;
    DWORD Size;
    DWORD Read = 0;
    SIZE_T Start = 0;

    Records.clear();
    File = CreateFileW(Path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;

    Size = GetFileSize(File, NULL);
    if (Size != INVALID_FILE_SIZE && Size >= sizeof(WCHAR))
    {
        Text.resize(Size / sizeof(WCHAR));
        ReadFile(File, &Text[0], Size, &Read, NULL);
        Text.resize(Read / sizeof(WCHAR));
    }
    CloseHandle(File);

    if (!Text.empty() && Text[0] == 0xFEFF)
        Text.erase(0, 1);

    while (Start < Text.size())
    {
        SIZE_T End = Text.find(L'\n', Start);
        std::wstring Line = Text.substr(Start, (End == std::wstring::npos) ? std::wstring::npos : End - Start);
        APP_RECORD Record;
        SIZE_T Field = 0;
        SIZE_T Tab;

        Start = (End == std::wstring::npos) ? Text.size() : End + 1;
        if (!Line.empty() && Line.back() == L'\r')
            Line.pop_back();
        if (Line.empty())
            continue;

        while ((Tab = Line.find(L'\t', Field)) != std::wstring::npos)
        {
            Record.push_back(Line.substr(Field, Tab - Field));
            Field = Tab + 1;
        }
        Record.push_back(Line.substr(Field));
        Records.push_back(Record);
    }
    return TRUE;
}

inline
BOOL
SaveRecords(
    _In_z_ PCWSTR Name,
    _In_ const std::vector<APP_RECORD> &Records)
{
    std::wstring Path = AppDataFile(Name);
    std::wstring Text(1, static_cast<WCHAR>(0xFEFF));
    HANDLE File;
    DWORD Written = 0;
    BOOL Result;

    for (const APP_RECORD &Record : Records)
    {
        for (SIZE_T Index = 0; Index < Record.size(); Index++)
        {
            std::wstring Field = Record[Index];

            /* Tabs and line breaks would split the record */
            for (WCHAR &Char : Field)
            {
                if (Char == L'\t' || Char == L'\n' || Char == L'\r')
                    Char = L' ';
            }
            if (Index)
                Text += L'\t';
            Text += Field;
        }
        Text += L"\r\n";
    }

    File = CreateFileW(Path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;

    Result = WriteFile(File, Text.data(), static_cast<DWORD>(Text.size() * sizeof(WCHAR)), &Written, NULL);
    CloseHandle(File);
    return Result;
}
