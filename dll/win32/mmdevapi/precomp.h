/*
 * PROJECT:     ReactOS Multimedia Device API
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private definitions of mmdevapi
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#define WIN32_NO_STATUS
#include <stdarg.h>
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <objbase.h>
#include <olectl.h>
#include <propidl.h>
#include <strsafe.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <audioclientactivationparams.h>
#include <audioengine.h>
#define NTOS_MODE_USER
#include <ndk/rtlfuncs.h>
#include <new>

#define NDEBUG
#include <debug.h>

#define MMD_MMDEVICES_KEY   L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio"

/* Default and minimum periods of the shared engine */
#define MMD_DEFAULT_PERIOD  100000
#define MMD_MINIMUM_PERIOD  30000

/* Volume range of an endpoint without hardware volume, and the steps VolumeStepUp takes */
#define MMD_VOLUME_MIN_DB   -96.0f
#define MMD_VOLUME_MAX_DB   0.0f
#define MMD_VOLUME_STEP_DB  1.5f
#define MMD_VOLUME_STEPS    51

extern HINSTANCE MmdInstance;
extern LONG MmdObjects;

/* Every COM object of the module counts itself so DllCanUnloadNow can tell */
class CMmdObject
{
public:
    CMmdObject() : m_Refs(1) { InterlockedIncrement(&MmdObjects); }
    virtual ~CMmdObject() { InterlockedDecrement(&MmdObjects); }

protected:
    ULONG InternalAddRef() { return InterlockedIncrement(&m_Refs); }

    ULONG
    InternalRelease()
    {
        ULONG Refs = InterlockedDecrement(&m_Refs);

        if (!Refs)
            delete this;
        return Refs;
    }

private:
    LONG m_Refs;
};

#define MMD_IMPLEMENT_REFCOUNT() \
    STDMETHODIMP_(ULONG) AddRef() override { return InternalAddRef(); } \
    STDMETHODIMP_(ULONG) Release() override { return InternalRelease(); }

/* engine.cpp */
VOID
MmdInitialize(VOID);

VOID
MmdUninitialize(VOID);

/* The published engine state, or NULL while audiosrv is not running */
const AE_STATE *
MmdGetState(VOID);

/* Changes each time the pipe to the engine is made again; streams of an older one are gone */
ULONG
MmdGetConnection(VOID);

HRESULT
MmdCall(
    _Inout_updates_bytes_(Size) AE_REQUEST *Request,
    _In_ ULONG Command,
    _In_ ULONG Size,
    _Out_writes_bytes_opt_(ReplySize) AE_REPLY *Reply,
    _In_ ULONG ReplySize);

ULONG
MmdFindEndpoint(
    _In_ PCWSTR Id);

BOOL
MmdIsEndpointActive(
    _In_ ULONG Endpoint);

/* What the watch thread tells a registered callback about */
typedef enum _MMD_WATCH_KIND
{
    MmdWatchEndpoints,
    MmdWatchEndpointVolume,
    MmdWatchSessionEvents,
    MmdWatchSessionCreated,
} MMD_WATCH_KIND;

HRESULT
MmdAddWatch(
    _In_ MMD_WATCH_KIND Kind,
    _In_ ULONG Index,
    _In_ ULONG Generation,
    _In_ IUnknown *Callback);

HRESULT
MmdRemoveWatch(
    _In_ MMD_WATCH_KIND Kind,
    _In_ ULONG Index,
    _In_ IUnknown *Callback);

/* enumerator.cpp */
HRESULT
MmdCreateEnumerator(
    _In_ REFIID Riid,
    _Outptr_ VOID **Object);

HRESULT
MmdCreateDevice(
    _In_ ULONG Endpoint,
    _Outptr_ IMMDevice **Device);

HRESULT
MmdActivateOnEndpoint(
    _In_ ULONG Endpoint,
    _In_ REFIID Riid,
    _In_opt_ PROPVARIANT *Parameters,
    _Outptr_ VOID **Object);

/* properties.cpp */
HRESULT
MmdCreatePropertyStore(
    _In_ ULONG Endpoint,
    _In_ DWORD Access,
    _Outptr_ IPropertyStore **Store);

/* client.cpp */
HRESULT
MmdCreateAudioClient(
    _In_ ULONG Endpoint,
    _In_ ULONG Kind,
    _In_ ULONG TargetProcessId,
    _In_ BOOL IncludeTree,
    _In_ REFIID Riid,
    _Outptr_ VOID **Object);

VOID
MmdMakeMixFormat(
    _In_ ULONG Endpoint,
    _Out_ WAVEFORMATEXTENSIBLE *Format);

/* session.cpp */
HRESULT
MmdCreateSessionManager(
    _In_ ULONG Endpoint,
    _In_ REFIID Riid,
    _Outptr_ VOID **Object);

HRESULT
MmdCreateSessionObject(
    _In_ ULONG Session,
    _In_ ULONG Generation,
    _In_ REFIID Riid,
    _Outptr_ VOID **Object);

/* endpointvolume.cpp */
HRESULT
MmdCreateEndpointVolume(
    _In_ ULONG Endpoint,
    _In_ REFIID Riid,
    _Outptr_ VOID **Object);

HRESULT
MmdCreateEndpointMeter(
    _In_ ULONG Endpoint,
    _In_ REFIID Riid,
    _Outptr_ VOID **Object);
