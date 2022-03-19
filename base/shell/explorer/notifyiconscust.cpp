/*
 * ReactOS Explorer
 *
 * Copyright 2015 Jared Smudde <computerwhiz02@hotmail.com>
 * Copyright 2018 Denis Malikov <filedem@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "precomp.h"

#include "tchar.h"
#include "psapi.h"


#define IconStreams L"IconStreams"
#define PastIconsStream L"PastIconsStream"

VOID ShowBehaviorCombo(HWND hwnd, LPNMITEMACTIVATE itemdata)
{
    HWND hListView = GetDlgItem(hwnd, IDC_NOTIFICATION_LIST);
    HWND hCombo = GetDlgItem(hwnd, IDC_NOTIFICATION_BEHAVIOUR);

    if (ComboBox_GetCount(hCombo) == 0)
    {
        WCHAR sShow[100];
        WCHAR sHide[100];
        WCHAR sInactive[100];
        LoadStringW(NULL, IDS_NOTIF_BEH_SHOW, sShow, _countof(sShow));
        LoadStringW(NULL, IDS_NOTIF_BEH_HIDE, sHide, _countof(sHide));
        LoadStringW(NULL, IDS_NOTIF_BEH_HIDE_INACTIVE, sInactive, _countof(sInactive));
        ComboBox_AddString(hCombo, sShow);
        ComboBox_AddString(hCombo, sHide);
        ComboBox_AddString(hCombo, sInactive);
        ComboBox_SetCurSel(hCombo, BEH_ALWAYS_SHOW);
    }

    int iItem = itemdata->iItem;
    if (iItem > -1)
    {
        POINT pt;
        RECT rc;
        ListView_GetItemPosition(hListView, iItem, &pt);
        GetWindowRect(hListView, &rc);
        POINT lv = { rc.left, rc.top };
        ScreenToClient(hwnd, &lv);

        LVITEM lvItem;
        lvItem.iItem = iItem;
        lvItem.iSubItem = 0;
        lvItem.mask = LVIF_PARAM;
        ListView_GetItem(hListView, &lvItem);

        ComboBox_SetCurSel(hCombo, lvItem.lParam);

        int iNameWidth = ListView_GetColumnWidth(hListView, 0);
        int iBehaviorWidth = ListView_GetColumnWidth(hListView, 1);

        ComboBox_SetItemData(hCombo, 0, iItem);
        SetWindowPos(hCombo, HWND_TOP, iNameWidth + lv.x, pt.y + lv.y, iBehaviorWidth, 20, SWP_SHOWWINDOW);
    }
    else
    {
        ShowWindow(hCombo, SW_HIDE);
    }
}

VOID SetItemBehavior(HWND hwnd, HWND hCombo)
{
    int iBehavior = ComboBox_GetCurSel(hCombo);
    int iItem = (int)ComboBox_GetItemData(hCombo, 0);
    HWND hListView = GetDlgItem(hwnd, IDC_NOTIFICATION_LIST);
    CNotifyToolbar* toolbar = (CNotifyToolbar*)GetWindowLongPtrW(hListView, GWLP_USERDATA);
    WCHAR szBehavior[100];
    int resId = iBehavior == BEH_ALWAYS_SHOW 
                    ? IDS_NOTIF_BEH_SHOW 
                    : (iBehavior == BEH_ALWAYS_HIDE ? IDS_NOTIF_BEH_HIDE : IDS_NOTIF_BEH_HIDE_INACTIVE);
    LoadStringW(NULL, resId, szBehavior, _countof(szBehavior));

    toolbar->SetBehavior(iItem, iBehavior);
    
    LVITEM lvItem;
    ZeroMemory(&lvItem, sizeof(lvItem));
    lvItem.mask = LVIF_TEXT;
    lvItem.iItem = iItem;
    lvItem.iSubItem = 1;
    lvItem.cchTextMax = 100;
    lvItem.pszText = szBehavior;
    ListView_SetItem(hListView, &lvItem);

    ZeroMemory(&lvItem, sizeof(lvItem));
    lvItem.mask = LVIF_PARAM;
    lvItem.iSubItem = 0;
    lvItem.lParam = iBehavior;
    lvItem.iItem = iItem;
    ListView_SetItem(hListView, &lvItem);

    ShowWindow(hCombo, SW_HIDE);
}

VOID ResetBehaviors(HWND hwnd)
{
    HWND hListView = GetDlgItem(hwnd, IDC_NOTIFICATION_LIST);
    CNotifyToolbar* toolbar = (CNotifyToolbar*)GetWindowLongPtrW(hListView, GWLP_USERDATA);
    int count = ListView_GetItemCount(hListView);
    WCHAR szBehavior[100];
    LoadStringW(NULL, IDS_NOTIF_BEH_SHOW, szBehavior, _countof(szBehavior));

    LVITEM lvi;
    ZeroMemory(&lvi, sizeof(lvi));
    lvi.cchTextMax = _countof(szBehavior);
    lvi.pszText = szBehavior;
    lvi.lParam = BEH_ALWAYS_SHOW;

    TBBUTTONINFO tbbi = { sizeof(tbbi) };
    tbbi.dwMask = TBIF_STATE | TBIF_LPARAM;

    for (int i = 0; i < count; i++)
    {
        toolbar->GetButtonInfo(i, &tbbi);
        
        if (((InternalIconData*)tbbi.lParam)->Locked == FALSE)
        {
            lvi.mask = LVIF_PARAM;
            lvi.iItem = i;
            lvi.iSubItem = 0;
            ListView_SetItem(hListView, &lvi);

            lvi.mask = LVIF_TEXT;
            lvi.iItem = i;
            lvi.iSubItem = 1;
            ListView_SetItem(hListView, &lvi);
        
            //tbbi.fsState &= ~TBSTATE_HIDDEN;
            //((InternalIconData*)tbbi.lParam)->dwState &= ~NIS_HIDDEN;
            
            //((InternalIconData*)tbbi.lParam)->uBehaviour = BEH_ALWAYS_SHOW;
        
            toolbar->SetBehavior(i, BEH_ALWAYS_SHOW);
        }
    }
    
    toolbar->Toogle(FALSE);
}

VOID InitializeListView(HWND hwnd)
{
    HWND hListView = GetDlgItem(hwnd, IDC_NOTIFICATION_LIST);

    LVCOLUMN lvColumn;
    WCHAR sName[256];
                
    SendMessage(hListView, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT);

    ZeroMemory(&lvColumn, sizeof(lvColumn));
    lvColumn.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                
    LoadStringW(NULL, IDS_NOTIF_BEH_NAME, sName, _countof(sName));
    lvColumn.pszText = sName;
    lvColumn.cx = 190;
    ListView_InsertColumn(hListView, 0, &lvColumn);

    LoadStringW(NULL, IDS_NOTIF_BEH_BEHAVIOR, sName, _countof(sName));
    lvColumn.pszText = sName;
    lvColumn.cx = 130;
    ListView_InsertColumn(hListView, 1, &lvColumn);

    ListView_EnableGroupView(hListView, TRUE);

    LVGROUP lvGroup;
    lvGroup.cbSize    = sizeof(LVGROUP);
    lvGroup.mask      = LVGF_HEADER | LVGF_ALIGN | LVGF_GROUPID | LVGF_STATE;
    lvGroup.uAlign    = LVGA_FOOTER_LEFT;
    lvGroup.state     = LVGS_NORMAL;

    LoadStringW(NULL, IDS_NOTIF_BEH_CURRENT, sName, _countof(sName));
    lvGroup.pszHeader = sName;
    lvGroup.iGroupId  = 0;
    ListView_InsertGroup(hListView, -1, &lvGroup);

    LoadStringW(NULL, IDS_NOTIF_BEH_PAST, sName, _countof(sName));
    lvGroup.pszHeader = sName;
    lvGroup.iGroupId  = 1;
    ListView_InsertGroup(hListView, -1, &lvGroup);
}

void ApplyAndSaveBehaviors(HWND hDlg)
{
    //::MessageBoxW(NULL, L"saving", L"no", MB_OK);
    HKEY subKey;
    LONG result = RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\TrayNotify\\test", 0, KEY_READ, &subKey);
    if (result != ERROR_SUCCESS)
    {
        DWORD dwDisposition;
        HKEY hKey;

        if (RegCreateKeyEx(HKEY_CURRENT_USER,
                           L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\TrayNotify\\test",
                           0,
                           NULL,
                           REG_OPTION_NON_VOLATILE,
                           KEY_ALL_ACCESS,
                           NULL,
                           &hKey,
                           &dwDisposition))
        {
            ::MessageBoxW(NULL, L"create failed", L"no", MB_OK);
            return;
        }
        else
            RegCloseKey(hKey);
    }

    ::MessageBoxW(NULL, L"saving2", L"no", MB_OK);

    IStream* reg = SHOpenRegStream2W(HKEY_CURRENT_USER,
                                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\TrayNotify",
                                     L"test2",
                                     STGM_WRITE);

    HWND hListView = GetDlgItem(hDlg, IDC_NOTIFICATION_LIST);
    HIMAGELIST hilPastIcons = ListView_GetImageList(hListView, LVSIL_SMALL);
    if (reg != NULL)
    {
        IconStreamsHeader ish;
        ULONG written;
		
		LARGE_INTEGER liPos;
        ULARGE_INTEGER uliSize;

        liPos.QuadPart = 0;
        uliSize.QuadPart = 1100000;

        // Set the size to 0 to delete the existing contents.
        reg->Seek ( liPos, STREAM_SEEK_SET, NULL );
        reg->SetSize ( uliSize );
		
        reg->Write(&ish, sizeof(ish), &written);

        if (written == 0) ::MessageBoxW(NULL, L"no header", L"no", MB_OK);

        if (ImageList_Write(hilPastIcons, reg) == FALSE)
            ::MessageBoxW(NULL, L"no images", L"no", MB_OK);
        

        //RegSetValueExW(hKey, L"View", 0, REG_BINARY, (LPBYTE)&tConfig, iBufferSize);
        //RegCloseKey(hKey);
        //reg->Commit(STGC_DEFAULT);
        reg->Release();
    }
    else
    {
        ::MessageBoxW(NULL, L"no test stream", L"no", MB_OK);
    }
}

INT_PTR CALLBACK CustomizeNotifyIconsProc(HWND hwnd, UINT Message, WPARAM wParam, LPARAM lParam)
{
    switch(Message)
    {
        case WM_INITDIALOG: 
            InitializeListView(hwnd);
            break;
        case WM_NOTIFY:
            if (LOWORD(wParam) == IDC_NOTIFICATION_LIST && ((LPNMHDR)lParam)->code == NM_CLICK) 
            {
                ShowBehaviorCombo(hwnd, (LPNMITEMACTIVATE)lParam);
            }
            else return FALSE;
            break;
        case WM_COMMAND:
            switch(LOWORD(wParam))
            {
                case IDOK:
                    if (1 == 0)
                        ApplyAndSaveBehaviors(hwnd);
                    EndDialog(hwnd, IDOK);
                    break;
                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    break;
                case IDC_NOTIFICATION_BEHAVIOUR:
                    if (HIWORD(wParam) == CBN_SELCHANGE)
                    {
                        SetItemBehavior(hwnd, (HWND)lParam);
                    }
                    break;
                case IDC_TASKBARPROP_NOTIREST:
                    ResetBehaviors(hwnd);
                    break;
            }
            break;
        default:
            return FALSE;
    }
    return TRUE;
}

bool CheckImageRunning(CNotifyToolbar* toolbar, LPWSTR exePath)
{
    int cnt = toolbar->GetButtonCount();
    TBBUTTON tbtn;
    WCHAR path[MAX_PATH];
    CString *exe = new CString(CharLower(exePath));
    DWORD dwDriveMap = GetLogicalDrives();
    WCHAR dosPath[3] = L"a:";
    WCHAR dosTarget[MAX_PATH];

    for (int i = 0; i < cnt; i++)
    {
        if (toolbar->GetButton(i, &tbtn))
        {
            InternalIconData *data = (InternalIconData*)tbtn.dwData;
            DWORD processId;
            GetWindowThreadProcessId(data->hWnd, &processId);

            HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, processId);
            UINT uiLen = GetProcessImageFileNameW(hProcess, path, MAX_PATH);
            
            if (uiLen > 0) 
            {
                CString *str = new CString(CharLower(path));
                
                for (DWORD drive = 0; drive < 26; drive++)
                if (dwDriveMap & (1<<drive))
                {
                    dosPath[0] = (WCHAR)('a' + drive);
                    
                    if (QueryDosDevice(dosPath, dosTarget, sizeof(dosTarget)) == 0) 
                        continue;

                    str->Replace(CharLower(dosTarget), dosPath);

                    if (*str == *exe)
                        return true;
                }
            }
        }
    }

    return false;
}

HIMAGELIST MergeImageLists(HIMAGELIST list1, HIMAGELIST list2)
{
    if (list1 == NULL && list2 == NULL) return NULL;

    HIMAGELIST hilMerged;

    if (list1 != NULL)
    {
        hilMerged = ImageList_Duplicate(list1);
    };

    if (list2 != NULL)
    {
        int imageCount = ImageList_GetImageCount(list2);
        for (int i = 0; i < imageCount; i++)
        {
            HICON hIco = ImageList_GetIcon(list2, i, ILD_NORMAL);
            ImageList_AddIcon(hilMerged, hIco);
        }
    }

    return hilMerged;
}

void DumpKey3(HWND hListView, char *tmp, CNotifyToolbar* toolbar, HIMAGELIST hilTrayIcons, HIMAGELIST hilPastIcons)
{
    //WCHAR str[MAX_PATH];

    // Merging image lists into one
    int pastImageCount = ImageList_GetImageCount(hilPastIcons);
    ListView_SetImageList(hListView, hilPastIcons, LVSIL_SMALL);
    
    LVITEM lvItem;
    for (int i = 0; i < pastImageCount; i++)
    {

        ZeroMemory(&lvItem, sizeof(lvItem));
        lvItem.mask = LVIF_IMAGE | LVIF_GROUPID;// | LVIF_PARAM;
        //lvItem.cchTextMax = _countof(((NOTIFYICONDATA*)tbbtn.dwData)->szTip);
        lvItem.iImage = i;
        lvItem.iItem = i;
        //lvItem.pszText = ((NOTIFYICONDATA*)tbbtn.dwData)->szTip;
        ListView_InsertItem(hListView, &lvItem);

    }

  
}

void DumpKey2(HWND hListView, CNotifyToolbar* toolbar)
{
    DWORD dwType = 0, dwLen = sizeof(IconStreamsItem) * 100 + sizeof(IconStreamsHeader);
    CComHeapPtr<char> tmp;
    tmp.AllocateBytes(dwLen);

    IStream* reg = SHOpenRegStream2W(HKEY_CURRENT_USER,
                                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\TrayNotify",
                                     PastIconsStream,
                                     STGM_READ);
    HIMAGELIST hilPastIcons;
    if (reg != NULL)
    {
        IconStreamsHeader ish;
        ULONG ulRead;
    
        reg->Read(&ish, sizeof(ish), &ulRead);
        hilPastIcons = ImageList_Read(reg);
        reg->Release();
    }
    else
    {
        ::MessageBoxW(NULL, L"no past stream", L"no", MB_OK);
    }

    if (hilPastIcons == NULL)
    {
        WCHAR error[20];
        _itow((int)hilPastIcons, error, 10);
        ::MessageBoxW(NULL, error, L"no past icons", MB_OK);
    }

    HIMAGELIST hilTrayIcons = (HIMAGELIST)toolbar->GetImageList();
    if (hilTrayIcons != NULL)
    {
        hilTrayIcons = ImageList_Duplicate(hilTrayIcons);
    }

    LSTATUS ls = SHGetValueW(HKEY_CURRENT_USER,
                             L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\TrayNotify",
                             IconStreams,
                             &dwType, tmp, &dwLen);

    if (!(SUCCEEDED(ls) && dwType == REG_BINARY))
    {
        ::MessageBoxW(NULL, L"no", L"no1", MB_OK);
    }

    DumpKey3(hListView, tmp, toolbar, hilTrayIcons, hilPastIcons);
}

void InitListView(HWND hListView, CNotifyToolbar* toolbar)
{
    SendMessage(hListView, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT);

    LVCOLUMN lvColumn;

    ZeroMemory(&lvColumn, sizeof(lvColumn));
    lvColumn.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    WCHAR name[5] = L"Name";
    int col = 0;
    lvColumn.pszText = name;
    lvColumn.cx = 130;
    ListView_InsertColumn(hListView, col++, &lvColumn);

    WCHAR path[5] = L"path";
    lvColumn.pszText = path;
    lvColumn.cx = 50;
    ListView_InsertColumn(hListView, col++, &lvColumn);

    WCHAR icon[5] = L"icon";
    lvColumn.pszText = icon;
    lvColumn.cx = 40;
    ListView_InsertColumn(hListView, col++, &lvColumn);

    WCHAR zero0[8] = L"zero 0";
    lvColumn.pszText = zero0;
    lvColumn.cx = 50;
    ListView_InsertColumn(hListView, col++, &lvColumn);

    WCHAR zero1[8] = L"zero 1";
    lvColumn.pszText = zero1;
    lvColumn.cx = 50;
    ListView_InsertColumn(hListView, col++, &lvColumn);

    WCHAR zero2[8] = L"behav";
    lvColumn.pszText = zero2;
    lvColumn.cx = 50;
    ListView_InsertColumn(hListView, col++, &lvColumn);

    ListView_EnableGroupView(hListView, TRUE);
    WCHAR sName[256];

    LVGROUP lvGroup;
    lvGroup.cbSize    = sizeof(LVGROUP);
    lvGroup.mask      = LVGF_HEADER | LVGF_ALIGN | LVGF_GROUPID | LVGF_STATE;
    lvGroup.uAlign    = LVGA_FOOTER_LEFT;
    lvGroup.state     = LVGS_NORMAL;

    LoadStringW(NULL, IDS_NOTIF_BEH_CURRENT, sName, _countof(sName));
    lvGroup.pszHeader = sName;
    lvGroup.iGroupId  = 0;
    ListView_InsertGroup(hListView, -1, &lvGroup);

    LoadStringW(NULL, IDS_NOTIF_BEH_PAST, sName, _countof(sName));
    lvGroup.pszHeader = sName;
    lvGroup.iGroupId  = 1;
    ListView_InsertGroup(hListView, -1, &lvGroup);

    DumpKey2(hListView, toolbar);
}

VOID SetNotifyIcons(HWND hDialog, IUnknown *TrayNotify)
{
    HWND hListView = GetDlgItem(hDialog, IDC_NOTIFICATION_LIST);    
    CNotifyToolbar* toolbar = CTrayNotifyWnd_GetTrayToolbar(TrayNotify);
    
    if (toolbar == NULL) return;
    
    SetWindowLongPtrW(hListView, GWLP_USERDATA, (LONG)toolbar);
    
    HIMAGELIST tbImageList = (HIMAGELIST)toolbar->GetImageList();
    HIMAGELIST lvImageList = ImageList_Duplicate(tbImageList);
    ListView_SetImageList(hListView, lvImageList, LVSIL_SMALL);
    
    WCHAR sShow[100];
    WCHAR sHide[100];
    WCHAR sInactive[100];
    LoadStringW(NULL, IDS_NOTIF_BEH_SHOW, sShow, _countof(sShow));
    LoadStringW(NULL, IDS_NOTIF_BEH_HIDE, sHide, _countof(sHide));
    LoadStringW(NULL, IDS_NOTIF_BEH_HIDE_INACTIVE, sInactive, _countof(sInactive));
    
    LVITEM lvItem;
    TBBUTTON tbtn;
    
    DWORD nButtons = toolbar->GetButtonCount();
    for (UINT i = 0; i < nButtons; i++)
    {
        if (toolbar->GetButton(i, &tbtn))
        {
            int bhv = ((InternalIconData*)tbtn.dwData)->uBehaviour;
            ZeroMemory(&lvItem, sizeof(lvItem));
            lvItem.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_GROUPID | LVIF_PARAM;
            lvItem.cchTextMax = 256;
            lvItem.iImage = tbtn.iBitmap;
            lvItem.iItem = i;
            lvItem.pszText = ((InternalIconData*)tbtn.dwData)->szTip;
            lvItem.lParam = bhv;
            ListView_InsertItem(hListView, &lvItem);
   
            ZeroMemory(&lvItem, sizeof(lvItem));
            lvItem.mask = LVIF_TEXT;
            lvItem.iItem = i;
            lvItem.iSubItem = 1;
            lvItem.cchTextMax = 100;
            lvItem.pszText = bhv == BEH_ALWAYS_SHOW ? sShow : (bhv == BEH_ALWAYS_HIDE ? sHide : sInactive);
            ListView_SetItem(hListView, &lvItem);
        }
    }
    //InitListView(hListView, toolbar);
}

VOID ShowCustomizeNotifyIcons(HINSTANCE hInst, HWND hExplorer, IUnknown *TrayNotify)
{
    HWND hDlg = CreateDialogW(hInst, MAKEINTRESOURCEW(IDD_NOTIFICATIONS_CUSTOMIZE), hExplorer, CustomizeNotifyIconsProc);

    SetNotifyIcons(hDlg, TrayNotify);

    ShowWindow(hDlg, SW_SHOW);
}