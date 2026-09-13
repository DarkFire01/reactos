/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tests for the DEVPROPKEY SetupDi and CM property functions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include <apitest.h>

#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <winuser.h>
#include <winreg.h>
#include <cfgmgr32.h>
#include <setupapi.h>
#include <initguid.h>
#include <devpkey.h>

#define TEST_FMTID { 0x9D3A1F42, 0x6C0B, 0x4E27, { 0xB8, 0x51, 0x2A, 0x7E, 0x0C, 0x93, 0x4D, 0x16 } }

static const DEVPROPKEY TestKeyNumber = { TEST_FMTID, 2 };
static const DEVPROPKEY TestKeyString = { TEST_FMTID, 3 };

static const GUID DiskInterfaceClass = { 0x53F56307, 0xB6BF, 0x11D0, { 0x94, 0xF2, 0x00, 0xA0, 0xC9, 0x1E, 0xFB, 0x8B } };
static const GUID MissingClass = { 0x9D3A1F42, 0x6C0B, 0x4E27, { 0xB8, 0x51, 0x2A, 0x7E, 0x0C, 0x93, 0x4D, 0x17 } };
static const GUID NullGuid = { 0 };

static BOOL CanWrite = TRUE;

static
BOOL
ContainsKey(
    _In_reads_(Count) const DEVPROPKEY *Keys,
    _In_ ULONG Count,
    _In_ const DEVPROPKEY *Key)
{
    ULONG i;

    for (i = 0; i < Count; i++)
    {
        if (Keys[i].pid == Key->pid && IsEqualGUID(&Keys[i].fmtid, &Key->fmtid))
            return TRUE;
    }

    return FALSE;
}

static
VOID
Test_DeviceSetupDi(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PSP_DEVINFO_DATA DeviceInfoData,
    _In_ PCWSTR InstanceId)
{
    WCHAR Buffer[MAX_DEVICE_ID_LEN];
    DEVPROPKEY *Keys;
    DEVPROPTYPE Type;
    DWORD Required, Count, Number = 0x1234ABCD, Value;
    DEVPROP_BOOLEAN Present;
    BOOL ret;

    Required = 0xdeadbeef;
    SetLastError(0xdeadbeef);
    ret = SetupDiGetDevicePropertyW(DeviceInfoSet, DeviceInfoData, &DEVPKEY_Device_InstanceId,
                                    &Type, NULL, 0, &Required, 0);
    ok(!ret, "Size query succeeded\n");
    ok_err(ERROR_INSUFFICIENT_BUFFER);
    ok_hex(Type, DEVPROP_TYPE_STRING);
    ok_int(Required, (wcslen(InstanceId) + 1) * sizeof(WCHAR));

    ret = SetupDiGetDevicePropertyW(DeviceInfoSet, DeviceInfoData, &DEVPKEY_Device_InstanceId,
                                    &Type, (PBYTE)Buffer, sizeof(Buffer), &Required, 0);
    ok(ret, "Failed to get the instance ID, error %lu\n", GetLastError());
    if (ret)
        ok(!_wcsicmp(Buffer, InstanceId), "Got '%S', expected '%S'\n", Buffer, InstanceId);

    Present = 0x55;
    ret = SetupDiGetDevicePropertyW(DeviceInfoSet, DeviceInfoData, &DEVPKEY_Device_IsPresent,
                                    &Type, (PBYTE)&Present, sizeof(Present), NULL, 0);
    ok(ret, "Failed to get IsPresent, error %lu\n", GetLastError());
    ok_hex(Type, DEVPROP_TYPE_BOOLEAN);
    ok_int(Present, DEVPROP_TRUE);

    SetLastError(0xdeadbeef);
    ret = SetupDiGetDevicePropertyW(DeviceInfoSet, DeviceInfoData, &DEVPKEY_Device_InstanceId,
                                    &Type, (PBYTE)Buffer, sizeof(Buffer), NULL, 1);
    ok(!ret, "Flags were accepted\n");
    ok_err(ERROR_INVALID_FLAGS);

    SetLastError(0xdeadbeef);
    ret = SetupDiGetDevicePropertyW(DeviceInfoSet, DeviceInfoData, NULL,
                                    &Type, (PBYTE)Buffer, sizeof(Buffer), NULL, 0);
    ok(!ret, "NULL key was accepted\n");
    ok_err(ERROR_INVALID_DATA);

    SetLastError(0xdeadbeef);
    ret = SetupDiGetDevicePropertyW(DeviceInfoSet, DeviceInfoData, &DEVPKEY_Device_InstanceId,
                                    &Type, NULL, sizeof(Buffer), NULL, 0);
    ok(!ret, "NULL buffer with a size was accepted\n");
    ok_err(ERROR_INVALID_USER_BUFFER);

    SetLastError(0xdeadbeef);
    ret = SetupDiGetDevicePropertyW(DeviceInfoSet, DeviceInfoData, &TestKeyNumber,
                                    &Type, (PBYTE)&Value, sizeof(Value), NULL, 0);
    ok(!ret, "Missing property was found\n");
    ok_err(ERROR_NOT_FOUND);

    SetLastError(0xdeadbeef);
    ret = SetupDiSetDevicePropertyW(DeviceInfoSet, DeviceInfoData, &TestKeyNumber,
                                    DEVPROP_TYPE_UINT32, (const BYTE *)&Number, sizeof(Number), 0);
    if (!ret && GetLastError() == ERROR_ACCESS_DENIED)
    {
        CanWrite = FALSE;
        skip("Not running as administrator\n");
        return;
    }
    ok(ret, "Failed to set the property, error %lu\n", GetLastError());

    Value = 0;
    ret = SetupDiGetDevicePropertyW(DeviceInfoSet, DeviceInfoData, &TestKeyNumber,
                                    &Type, (PBYTE)&Value, sizeof(Value), &Required, 0);
    ok(ret, "Failed to get the property, error %lu\n", GetLastError());
    ok_hex(Type, DEVPROP_TYPE_UINT32);
    ok_int(Required, sizeof(Value));
    ok_hex(Value, Number);

    SetLastError(0xdeadbeef);
    ret = SetupDiGetDevicePropertyKeys(DeviceInfoSet, DeviceInfoData, NULL, 0, &Count, 0);
    ok(!ret, "Key size query succeeded\n");
    ok_err(ERROR_INSUFFICIENT_BUFFER);
    ok(Count >= 2, "Got %lu keys\n", Count);

    Keys = HeapAlloc(GetProcessHeap(), 0, Count * sizeof(DEVPROPKEY));
    if (Keys)
    {
        DWORD Allocated = Count;

        ret = SetupDiGetDevicePropertyKeys(DeviceInfoSet, DeviceInfoData, Keys, Allocated, &Count, 0);
        ok(ret, "Failed to get %lu property keys, error %lu, required %lu\n",
           Allocated, GetLastError(), Count);
        if (ret)
        {
            ok(ContainsKey(Keys, Count, &TestKeyNumber), "Test key is not listed\n");
            ok(ContainsKey(Keys, Count, &DEVPKEY_Device_InstanceId), "Instance ID key is not listed\n");
        }
        HeapFree(GetProcessHeap(), 0, Keys);
    }

    SetLastError(0xdeadbeef);
    ret = SetupDiSetDevicePropertyW(DeviceInfoSet, DeviceInfoData, &TestKeyString,
                                    DEVPROP_TYPE_STRING, (const BYTE *)L"abc", 3 * sizeof(WCHAR), 0);
    ok(!ret, "Unterminated string was accepted\n");
    ok_err(ERROR_INVALID_DATA);

    SetLastError(0xdeadbeef);
    ret = SetupDiSetDevicePropertyW(DeviceInfoSet, DeviceInfoData, &DEVPKEY_Device_InstanceId,
                                    DEVPROP_TYPE_STRING, (const BYTE *)L"x", sizeof(L"x"), 0);
    ok(!ret, "Protected property was written\n");
    ok_err(ERROR_ACCESS_DENIED);

    ret = SetupDiSetDevicePropertyW(DeviceInfoSet, DeviceInfoData, &TestKeyNumber,
                                    DEVPROP_TYPE_EMPTY, NULL, 0, 0);
    ok(ret, "Failed to delete the property, error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    ret = SetupDiGetDevicePropertyW(DeviceInfoSet, DeviceInfoData, &TestKeyNumber,
                                    &Type, (PBYTE)&Value, sizeof(Value), NULL, 0);
    ok(!ret, "Deleted property was found\n");
    ok_err(ERROR_NOT_FOUND);
}

static
VOID
Test_DeviceCm(
    _In_ PCWSTR InstanceId,
    _In_ const GUID *ClassGuid)
{
    static const WCHAR String[] = L"cfgmgr32 property";
    WCHAR Buffer[64];
    DEVPROPKEY Keys[64];
    DEVPROPTYPE Type;
    DEVINST DevInst;
    GUID Guid;
    ULONG Size, Count;
    CONFIGRET cr;

    cr = CM_Locate_DevNodeW(&DevInst, (DEVINSTID_W)InstanceId, CM_LOCATE_DEVNODE_NORMAL);
    ok_hex(cr, CR_SUCCESS);
    if (cr != CR_SUCCESS)
        return;

    Size = 0;
    cr = CM_Get_DevNode_PropertyW(DevInst, &DEVPKEY_Device_InstanceId, &Type, NULL, &Size, 0);
    ok_hex(cr, CR_BUFFER_SMALL);
    ok_hex(Type, DEVPROP_TYPE_STRING);
    ok_int(Size, (wcslen(InstanceId) + 1) * sizeof(WCHAR));

    Size = sizeof(Buffer);
    ok_hex(CM_Get_DevNode_PropertyW(DevInst, &DEVPKEY_Device_InstanceId, &Type, NULL, &Size, 0), CR_INVALID_POINTER);
    ok_hex(CM_Get_DevNode_PropertyW(DevInst, &DEVPKEY_Device_InstanceId, &Type, (PBYTE)Buffer, &Size, 1), CR_INVALID_FLAG);
    ok_hex(CM_Get_DevNode_PropertyW(0, &DEVPKEY_Device_InstanceId, &Type, (PBYTE)Buffer, &Size, 0), CR_INVALID_DEVNODE);

    if (!IsEqualGUID(ClassGuid, &NullGuid))
    {
        Size = sizeof(Guid);
        cr = CM_Get_DevNode_PropertyW(DevInst, &DEVPKEY_Device_ClassGuid, &Type, (PBYTE)&Guid, &Size, 0);
        ok_hex(cr, CR_SUCCESS);
        ok_hex(Type, DEVPROP_TYPE_GUID);
        ok(IsEqualGUID(&Guid, ClassGuid), "Wrong class GUID\n");
    }

    if (!CanWrite)
        return;

    cr = CM_Set_DevNode_PropertyW(DevInst, &TestKeyString, DEVPROP_TYPE_STRING,
                                  (PBYTE)String, sizeof(String), 0);
    ok_hex(cr, CR_SUCCESS);

    Size = sizeof(Buffer);
    cr = CM_Get_DevNode_PropertyW(DevInst, &TestKeyString, &Type, (PBYTE)Buffer, &Size, 0);
    ok_hex(cr, CR_SUCCESS);
    ok_hex(Type, DEVPROP_TYPE_STRING);
    ok_int(Size, sizeof(String));
    ok(!wcscmp(Buffer, String), "Got '%S'\n", Buffer);

    Count = 0;
    cr = CM_Get_DevNode_Property_Keys(DevInst, NULL, &Count, 0);
    ok_hex(cr, CR_BUFFER_SMALL);

    Count = ARRAYSIZE(Keys);
    cr = CM_Get_DevNode_Property_Keys(DevInst, Keys, &Count, 0);
    ok_hex(cr, CR_SUCCESS);
    if (cr == CR_SUCCESS)
        ok(ContainsKey(Keys, Count, &TestKeyString), "Test key is not listed\n");

    cr = CM_Set_DevNode_PropertyW(DevInst, &TestKeyString, DEVPROP_TYPE_NULL, NULL, 0, 0);
    ok_hex(cr, CR_SUCCESS);

    Size = sizeof(Buffer);
    cr = CM_Get_DevNode_PropertyW(DevInst, &TestKeyString, &Type, (PBYTE)Buffer, &Size, 0);
    ok_hex(cr, CR_SUCCESS);
    ok_hex(Type, DEVPROP_TYPE_NULL);
    ok_int(Size, 0);

    cr = CM_Set_DevNode_PropertyW(DevInst, &TestKeyString, DEVPROP_TYPE_EMPTY, NULL, 0, 0);
    ok_hex(cr, CR_SUCCESS);

    Size = sizeof(Buffer);
    cr = CM_Get_DevNode_PropertyW(DevInst, &TestKeyString, &Type, (PBYTE)Buffer, &Size, 0);
    ok_hex(cr, CR_NO_SUCH_VALUE);
}

static
VOID
Test_Class(
    _In_ const GUID *ClassGuid)
{
    static const WCHAR String[] = L"class property";
    WCHAR Buffer[64];
    DEVPROPTYPE Type;
    DWORD Required, Count;
    ULONG Size;
    BOOL ret;

    ret = SetupDiGetClassPropertyW(ClassGuid, &DEVPKEY_DeviceClass_ClassName, &Type,
                                   (PBYTE)Buffer, sizeof(Buffer), &Required, DICLASSPROP_INSTALLER);
    ok(ret, "Failed to get the class name, error %lu\n", GetLastError());
    ok_hex(Type, DEVPROP_TYPE_STRING);

    SetLastError(0xdeadbeef);
    ret = SetupDiGetClassPropertyW(ClassGuid, &DEVPKEY_DeviceClass_ClassName, &Type,
                                   (PBYTE)Buffer, sizeof(Buffer), &Required, 0);
    ok(!ret, "Missing flags were accepted\n");
    ok_err(ERROR_INVALID_FLAGS);

    SetLastError(0xdeadbeef);
    ret = SetupDiGetClassPropertyExW(ClassGuid, &DEVPKEY_DeviceClass_ClassName, &Type,
                                     (PBYTE)Buffer, sizeof(Buffer), &Required,
                                     DICLASSPROP_INSTALLER, NULL, (PVOID)1);
    ok(!ret, "Reserved value was accepted\n");
    ok_err(ERROR_INVALID_PARAMETER);

    Size = sizeof(Buffer);
    ok_hex(CM_Get_Class_PropertyW(ClassGuid, &DEVPKEY_DeviceClass_ClassName, &Type, (PBYTE)Buffer, &Size, 2), CR_INVALID_FLAG);

    Size = sizeof(Buffer);
    ok_hex(CM_Get_Class_PropertyW(&MissingClass, &DEVPKEY_DeviceClass_ClassName, &Type, (PBYTE)Buffer, &Size, 0), CR_NO_SUCH_REGISTRY_KEY);

    if (!CanWrite)
        return;

    ret = SetupDiSetClassPropertyW(ClassGuid, &TestKeyString, DEVPROP_TYPE_STRING,
                                   (const BYTE *)String, sizeof(String), DICLASSPROP_INSTALLER);
    ok(ret, "Failed to set the class property, error %lu\n", GetLastError());

    Size = sizeof(Buffer);
    ok_hex(CM_Get_Class_PropertyW(ClassGuid, &TestKeyString, &Type, (PBYTE)Buffer, &Size, CM_CLASS_PROPERTY_INSTALLER), CR_SUCCESS);
    ok_hex(Type, DEVPROP_TYPE_STRING);
    ok(!wcscmp(Buffer, String), "Got '%S'\n", Buffer);

    SetLastError(0xdeadbeef);
    ret = SetupDiGetClassPropertyKeys(ClassGuid, NULL, 0, &Count, DICLASSPROP_INSTALLER);
    ok(!ret, "Key size query succeeded\n");
    ok_err(ERROR_INSUFFICIENT_BUFFER);
    ok(Count >= 1, "Got %lu keys\n", Count);

    ok_hex(CM_Set_Class_PropertyW(ClassGuid, &TestKeyString, DEVPROP_TYPE_EMPTY, NULL, 0, CM_CLASS_PROPERTY_INSTALLER), CR_SUCCESS);

    SetLastError(0xdeadbeef);
    ret = SetupDiGetClassPropertyW(ClassGuid, &TestKeyString, &Type, (PBYTE)Buffer,
                                   sizeof(Buffer), NULL, DICLASSPROP_INSTALLER);
    ok(!ret, "Deleted class property was found\n");
    ok_err(ERROR_NOT_FOUND);
}

static
VOID
Test_Interface(VOID)
{
    static const WCHAR String[] = L"interface property";
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W Detail;
    SP_DEVICE_INTERFACE_DATA InterfaceData;
    DEVPROP_BOOLEAN Enabled;
    WCHAR Buffer[64];
    DEVPROPTYPE Type;
    HDEVINFO DeviceInfoSet;
    DWORD Required;
    ULONG Size;
    GUID Guid;
    BOOL ret;

    Size = sizeof(Buffer);
    ok_hex(CM_Get_Device_Interface_PropertyW(L"\\\\?\\NOSUCH#DEVICE#0#{53f56307-b6bf-11d0-94f2-00a0c91efb8b}",
                                             &TestKeyString, &Type, (PBYTE)Buffer, &Size, 0),
           CR_NO_SUCH_DEVICE_INTERFACE);

    DeviceInfoSet = SetupDiGetClassDevsW(&DiskInterfaceClass, NULL, NULL,
                                         DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (DeviceInfoSet == INVALID_HANDLE_VALUE)
    {
        skip("No disk interfaces\n");
        return;
    }

    InterfaceData.cbSize = sizeof(InterfaceData);
    if (!SetupDiEnumDeviceInterfaces(DeviceInfoSet, NULL, &DiskInterfaceClass, 0, &InterfaceData))
    {
        skip("No disk interfaces\n");
        SetupDiDestroyDeviceInfoList(DeviceInfoSet);
        return;
    }

    ret = SetupDiGetDeviceInterfacePropertyW(DeviceInfoSet, &InterfaceData, &DEVPKEY_DeviceInterface_ClassGuid,
                                             &Type, (PBYTE)&Guid, sizeof(Guid), &Required, 0);
    ok(ret, "Failed to get the interface class, error %lu\n", GetLastError());
    ok_hex(Type, DEVPROP_TYPE_GUID);
    ok(IsEqualGUID(&Guid, &DiskInterfaceClass), "Wrong interface class\n");

    Enabled = 0x55;
    ret = SetupDiGetDeviceInterfacePropertyW(DeviceInfoSet, &InterfaceData, &DEVPKEY_DeviceInterface_Enabled,
                                             &Type, (PBYTE)&Enabled, sizeof(Enabled), NULL, 0);
    ok(ret, "Failed to get the enabled state, error %lu\n", GetLastError());
    ok_int(Enabled, DEVPROP_TRUE);

    if (!CanWrite)
        goto Cleanup;

    ret = SetupDiSetDeviceInterfacePropertyW(DeviceInfoSet, &InterfaceData, &TestKeyString, DEVPROP_TYPE_STRING,
                                             (const BYTE *)String, sizeof(String), 0);
    ok(ret, "Failed to set the interface property, error %lu\n", GetLastError());

    SetupDiGetDeviceInterfaceDetailW(DeviceInfoSet, &InterfaceData, NULL, 0, &Required, NULL);
    Detail = HeapAlloc(GetProcessHeap(), 0, Required);
    if (Detail)
    {
        Detail->cbSize = sizeof(*Detail);
        if (SetupDiGetDeviceInterfaceDetailW(DeviceInfoSet, &InterfaceData, Detail, Required, NULL, NULL))
        {
            Size = sizeof(Buffer);
            ok_hex(CM_Get_Device_Interface_PropertyW(Detail->DevicePath, &TestKeyString, &Type,
                                                     (PBYTE)Buffer, &Size, 0), CR_SUCCESS);
            ok_hex(Type, DEVPROP_TYPE_STRING);
            ok(!wcscmp(Buffer, String), "Got '%S'\n", Buffer);
        }
        HeapFree(GetProcessHeap(), 0, Detail);
    }

    ret = SetupDiSetDeviceInterfacePropertyW(DeviceInfoSet, &InterfaceData, &TestKeyString,
                                             DEVPROP_TYPE_EMPTY, NULL, 0, 0);
    ok(ret, "Failed to delete the interface property, error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    ret = SetupDiGetDeviceInterfacePropertyW(DeviceInfoSet, &InterfaceData, &TestKeyString,
                                             &Type, (PBYTE)Buffer, sizeof(Buffer), NULL, 0);
    ok(!ret, "Deleted interface property was found\n");
    ok_err(ERROR_NOT_FOUND);

Cleanup:
    SetupDiDestroyDeviceInfoList(DeviceInfoSet);
}

START_TEST(PropertyStore)
{
    WCHAR InstanceId[MAX_DEVICE_ID_LEN];
    SP_DEVINFO_DATA DeviceInfoData;
    HDEVINFO DeviceInfoSet;
    DWORD Index;
    BOOL Found = FALSE;

    DeviceInfoSet = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    ok(DeviceInfoSet != INVALID_HANDLE_VALUE, "SetupDiGetClassDevsW failed, error %lu\n", GetLastError());
    if (DeviceInfoSet == INVALID_HANDLE_VALUE)
        return;

    DeviceInfoData.cbSize = sizeof(DeviceInfoData);
    for (Index = 0; SetupDiEnumDeviceInfo(DeviceInfoSet, Index, &DeviceInfoData); Index++)
    {
        if (!SetupDiGetDeviceInstanceIdW(DeviceInfoSet, &DeviceInfoData, InstanceId,
                                         ARRAYSIZE(InstanceId), NULL))
            continue;

        if (_wcsicmp(InstanceId, L"HTREE\\ROOT\\0") && !IsEqualGUID(&DeviceInfoData.ClassGuid, &NullGuid))
        {
            Found = TRUE;
            break;
        }
    }

    if (Found)
    {
        trace("Using %S\n", InstanceId);
        Test_DeviceSetupDi(DeviceInfoSet, &DeviceInfoData, InstanceId);
        Test_DeviceCm(InstanceId, &DeviceInfoData.ClassGuid);
        Test_Class(&DeviceInfoData.ClassGuid);
    }
    else
    {
        skip("No installed device found\n");
    }

    SetupDiDestroyDeviceInfoList(DeviceInfoSet);

    Test_Interface();
}
