/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The class server the emulated devices are handed out by
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * The devices a machine of this kind has had since before anyone needed to
 * write a driver for them: the interrupt controllers, the interval timer, the
 * transfer controller and the keyboard controller.
 *
 * They are handed out by identifier rather than by name, so a manager that
 * wants one of these does not have to know which library it came out of, and a
 * library that has one can be put in front of this one.
 */

#define INITGUID

#include <windows.h>
#include <initguid.h>
#include <objbase.h>

#include "picdevice.h"
#include "dmadevice.h"
#include "pitdevice.h"
#include "videodevice.h"
#include "idedevice.h"
#include "pcibusdevice.h"

namespace rtvm
{

/* WHAT MAKES ONE *************************************************************/

typedef IVirtualDevice *(*PFN_MAKE_DEVICE)(void);

static IVirtualDevice *MakePicDevice(void)
{
    return static_cast<IVirtualDevice *>(new PicDevice());
}

static IVirtualDevice *MakeDmaDevice(void)
{
    return static_cast<IVirtualDevice *>(new DmaDevice());
}

static IVirtualDevice *MakePitDevice(void)
{
    return static_cast<IVirtualDevice *>(new PitDevice());
}

static IVirtualDevice *MakeVideoDevice(void)
{
    return static_cast<IVirtualDevice *>(new VideoDevice());
}

static IVirtualDevice *MakeIdeDevice(void)
{
    return static_cast<IVirtualDevice *>(new IdeControllerDevice());
}

static IVirtualDevice *MakePciBusDevice(void)
{
    return static_cast<IVirtualDevice *>(new PciBusDevice());
}

/*
 * Every kind this library has. A manager walks nothing but this, and adding a
 * device is adding a line to it.
 */
static const struct
{
    const GUID *Class;
    PFN_MAKE_DEVICE Make;
    const char *Name;
} DeviceMap[] =
{
    { &CLSID_PicDevice, MakePicDevice, "interrupt controller" },
    { &CLSID_DmaControllerDevice, MakeDmaDevice, "transfer controller" },
    { &CLSID_PitDevice, MakePitDevice, "interval timer" },
    { &CLSID_VideoS3Device, MakeVideoDevice, "display" },
    { &CLSID_IdeControllerDevice, MakeIdeDevice, "disk controller" },
    { &CLSID_PciBusDevice, MakePciBusDevice, "bus" }
};

/**
 * @brief
 * The thing a manager is given when it asks for a kind, and asks to make one.
 *
 * @remarks
 * One of these per kind, made on the way out and let go straight after, because
 * there is nothing in it worth keeping between one device and the next.
 */
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

/* WHAT THE LIBRARY OFFERS ****************************************************/

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

/*
 * A manager here reaches this library by path and by identifier, never through
 * the registry, so there is nothing to write into it. The export is here
 * because a library of this kind has it.
 */
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
