/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The class server the chipset devices are handed out by
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * The parts of a machine that are not devices anyone plugged in: where a line
 * goes, what the time is, and what happens when the power button is pressed.
 */

#define INITGUID

#include <windows.h>
#include <initguid.h>
#include <objbase.h>

#include "ioapicdevice.h"

namespace rtvm
{

typedef IVirtualDevice *(*PFN_MAKE_DEVICE)(void);

static IVirtualDevice *MakeIoApicDevice(void)
{
    return static_cast<IVirtualDevice *>(new IoApicDevice());
}

static const struct
{
    const GUID *Class;
    PFN_MAKE_DEVICE Make;
    const char *Name;
} DeviceMap[] =
{
    { &CLSID_IoApicDevice, MakeIoApicDevice, "line router" }
};

class DeviceFactory : public IClassFactory
{
public:
    explicit DeviceFactory(PFN_MAKE_DEVICE Make) noexcept : m_Make(Make) {}

    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override
    {
        if (Object == nullptr)
            return E_POINTER;

        if (IsEqualIID(Interface, IID_IUnknown) ||
            IsEqualIID(Interface, IID_IClassFactory))
        {
            *Object = static_cast<IClassFactory *>(this);
            AddRef();
            return S_OK;
        }

        *Object = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return (ULONG)InterlockedIncrement(&m_Count);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        const LONG Left = InterlockedDecrement(&m_Count);

        if (Left == 0)
            delete this;

        return (ULONG)Left;
    }

    STDMETHODIMP CreateInstance(IUnknown *Outer, REFIID Interface,
                                void **Object) override
    {
        if (Object == nullptr)
            return E_POINTER;

        *Object = nullptr;

        /* Nothing here is worth putting inside something else */
        if (Outer != nullptr)
            return CLASS_E_NOAGGREGATION;

        IVirtualDevice *Device = m_Make();

        if (Device == nullptr)
            return E_OUTOFMEMORY;

        const HRESULT Status = Device->QueryInterface(Interface, Object);

        Device->Release();
        return Status;
    }

    STDMETHODIMP LockServer(BOOL Lock) override
    {
        if (Lock)
            InterlockedIncrement(&g_Outstanding);
        else
            InterlockedDecrement(&g_Outstanding);

        return S_OK;
    }

private:
    volatile LONG m_Count = 1;
    PFN_MAKE_DEVICE m_Make;
};

} /* namespace rtvm */

STDAPI DllGetClassObject(
    _In_ REFCLSID Class,
    _In_ REFIID Interface,
    _Outptr_ LPVOID *Object)
{
    if (Object == NULL)
        return E_POINTER;

    *Object = NULL;

    for (const auto &Entry : rtvm::DeviceMap)
    {
        if (!IsEqualCLSID(Class, *Entry.Class))
            continue;

        auto *Factory = new rtvm::DeviceFactory(Entry.Make);

        if (Factory == NULL)
            return E_OUTOFMEMORY;

        const HRESULT Status = Factory->QueryInterface(Interface, Object);

        Factory->Release();
        return Status;
    }

    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow(VOID)
{
    return (rtvm::g_Outstanding == 0) ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer(VOID)
{
    return S_OK;
}

STDAPI DllUnregisterServer(VOID)
{
    return S_OK;
}

BOOL
WINAPI
DllMain(
    _In_ HINSTANCE Instance,
    _In_ DWORD Reason,
    _In_ LPVOID Reserved)
{
    UNREFERENCED_PARAMETER(Reserved);

    if (Reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(Instance);

    return TRUE;
}
