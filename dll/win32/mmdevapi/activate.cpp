/*
 * PROJECT:     ReactOS Multimedia Device API
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ActivateAudioInterfaceAsync
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

/* Device interface paths look like \\?\SWD#MMDEVAPI#{0.0.0.00000000}.{guid}#{interface class} */
#define MMD_SWD_PREFIX      L"\\\\?\\SWD#MMDEVAPI#"

static const IID MmdIidAgileObject =
    { 0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90 } };

class CActivateOperation
    : public CMmdObject
    , public IActivateAudioInterfaceAsyncOperation
{
public:
    CActivateOperation()
        : m_Done(FALSE)
        , m_Result(E_FAIL)
        , m_Interface(NULL)
    {
    }

    ~CActivateOperation()
    {
        if (m_Interface)
            m_Interface->Release();
    }

    VOID
    Complete(
        _In_ HRESULT Result,
        _In_opt_ IUnknown *Interface)
    {
        m_Result = Result;
        m_Interface = Interface;
        MemoryBarrier();
        m_Done = TRUE;
    }

    STDMETHODIMP
    QueryInterface(
        _In_ REFIID Riid,
        _Outptr_ VOID **Object) override
    {
        if (!Object)
            return E_POINTER;
        if (IsEqualIID(Riid, IID_IUnknown) || IsEqualIID(Riid, IID_IActivateAudioInterfaceAsyncOperation) ||
            IsEqualIID(Riid, MmdIidAgileObject))
        {
            *Object = static_cast<IActivateAudioInterfaceAsyncOperation *>(this);
            AddRef();
            return S_OK;
        }
        *Object = NULL;
        return E_NOINTERFACE;
    }

    MMD_IMPLEMENT_REFCOUNT()

    STDMETHODIMP
    GetActivateResult(
        _Out_ HRESULT *Result,
        _Outptr_result_maybenull_ IUnknown **Interface) override
    {
        if (!Result || !Interface)
            return E_POINTER;
        *Interface = NULL;
        if (!m_Done)
            return E_ILLEGAL_METHOD_CALL;

        *Result = m_Result;
        *Interface = m_Interface;
        if (m_Interface)
            m_Interface->AddRef();
        return S_OK;
    }

private:
    volatile BOOL m_Done;
    HRESULT m_Result;
    IUnknown *m_Interface;
};

typedef struct _MMD_ACTIVATION
{
    PWSTR Path;
    IID Riid;
    PROPVARIANT Parameters;
    IActivateAudioInterfaceCompletionHandler *Handler;
    CActivateOperation *Operation;
} MMD_ACTIVATION, *PMMD_ACTIVATION;

static
HRESULT
MmdActivateProcessLoopback(
    _In_ REFIID Riid,
    _In_ const PROPVARIANT *Parameters,
    _Outptr_ VOID **Object)
{
    const AUDIOCLIENT_ACTIVATION_PARAMS *Activation;
    const AE_STATE *State;
    ULONG Endpoint;

    if (Parameters->vt != VT_BLOB)
        return E_INVALIDARG;
    if (!IsEqualIID(Riid, IID_IAudioClient))
        return E_NOINTERFACE;
    if (Parameters->blob.cbSize != sizeof(AUDIOCLIENT_ACTIVATION_PARAMS))
        return E_INVALIDARG;

    Activation = (const AUDIOCLIENT_ACTIVATION_PARAMS *)Parameters->blob.pBlobData;
    if (Activation->ActivationType != AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK)
        return E_INVALIDARG;
    if (Activation->ProcessLoopbackParams.ProcessLoopbackMode != PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE &&
        Activation->ProcessLoopbackParams.ProcessLoopbackMode != PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE)
    {
        return E_INVALIDARG;
    }
    if (!Activation->ProcessLoopbackParams.TargetProcessId)
        return E_INVALIDARG;

    /* The engine listens on whatever the default render endpoint is when the stream opens */
    State = MmdGetState();
    Endpoint = State ? State->DefaultEndpoint[AE_FLOW_RENDER][eConsole] : AE_NO_ENDPOINT;
    return MmdCreateAudioClient(Endpoint, AE_STREAM_PROCESS_LOOPBACK,
                                Activation->ProcessLoopbackParams.TargetProcessId,
                                Activation->ProcessLoopbackParams.ProcessLoopbackMode ==
                                    PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE,
                                Riid, Object);
}

/* Default endpoint by interface class, or the endpoint a device interface path names */
static
HRESULT
MmdResolvePath(
    _In_ PCWSTR Path,
    _Out_ ULONG *Endpoint,
    _Out_ BOOL *Default)
{
    const AE_STATE *State = MmdGetState();
    WCHAR Id[AE_ID_LENGTH];
    PCWSTR Start, End;
    GUID Class;
    ULONG Flow;

    *Endpoint = AE_NO_ENDPOINT;
    *Default = FALSE;

    if (Path[0] == L'{' && wcslen(Path) == 38 && SUCCEEDED(CLSIDFromString(Path, &Class)))
    {
        if (IsEqualGUID(Class, DEVINTERFACE_AUDIO_RENDER))
            Flow = AE_FLOW_RENDER;
        else if (IsEqualGUID(Class, DEVINTERFACE_AUDIO_CAPTURE))
            Flow = AE_FLOW_CAPTURE;
        else
            return E_INVALIDARG;

        *Default = TRUE;
        *Endpoint = State ? State->DefaultEndpoint[Flow][eConsole] : AE_NO_ENDPOINT;
        return (*Endpoint == AE_NO_ENDPOINT) ? E_NOTFOUND : S_OK;
    }

    Start = Path;
    if (!_wcsnicmp(Path, MMD_SWD_PREFIX, wcslen(MMD_SWD_PREFIX)))
    {
        Start = Path + wcslen(MMD_SWD_PREFIX);
        End = wcschr(Start, L'#');
        if (!End || End - Start >= AE_ID_LENGTH)
            return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        CopyMemory(Id, Start, (End - Start) * sizeof(WCHAR));
        Id[End - Start] = UNICODE_NULL;
        Start = Id;
    }

    *Endpoint = MmdFindEndpoint(Start);
    return (*Endpoint == AE_NO_ENDPOINT) ? HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) : S_OK;
}

static
HRESULT
MmdActivate(
    _In_ PMMD_ACTIVATION Activation,
    _Outptr_ IUnknown **Interface)
{
    PROPVARIANT *Parameters = &Activation->Parameters;
    ULONG Endpoint;
    BOOL Default;
    HRESULT Hr;

    *Interface = NULL;
    if (!wcscmp(Activation->Path, VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK))
        return MmdActivateProcessLoopback(Activation->Riid, Parameters, (VOID **)Interface);

    Hr = MmdResolvePath(Activation->Path, &Endpoint, &Default);
    if (FAILED(Hr))
        return Hr;

    /* Clients of the default endpoint take no parameters, except a GUID sized blob for IAudioClient3 */
    if (Default && Parameters->vt != VT_EMPTY)
    {
        if (IsEqualIID(Activation->Riid, IID_IAudioClient3) && Parameters->vt == VT_BLOB &&
            Parameters->blob.cbSize == sizeof(GUID))
        {
            Parameters = NULL;
        }
        else if (IsEqualIID(Activation->Riid, IID_IAudioClient) || IsEqualIID(Activation->Riid, IID_IAudioClient2))
        {
            Parameters = NULL;
        }
        else
        {
            return E_INVALIDARG;
        }
    }

    return MmdActivateOnEndpoint(Endpoint, Activation->Riid, Parameters, (VOID **)Interface);
}

static
DWORD
WINAPI
MmdActivationWorker(
    _In_ LPVOID Context)
{
    PMMD_ACTIVATION Activation = (PMMD_ACTIVATION)Context;
    IUnknown *Interface;
    HRESULT Hr, Init;

    Init = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    Hr = MmdActivate(Activation, &Interface);
    Activation->Operation->Complete(Hr, SUCCEEDED(Hr) ? Interface : NULL);
    Activation->Handler->ActivateCompleted(Activation->Operation);

    Activation->Handler->Release();
    Activation->Operation->Release();
    PropVariantClear(&Activation->Parameters);
    HeapFree(GetProcessHeap(), 0, Activation->Path);
    HeapFree(GetProcessHeap(), 0, Activation);

    if (SUCCEEDED(Init))
        CoUninitialize();
    return 0;
}

/**
 * @brief
 * Activates an audio interface on a device interface path, on a worker thread.
 */
EXTERN_C
HRESULT
WINAPI
ActivateAudioInterfaceAsync(
    _In_ LPCWSTR DeviceInterfacePath,
    _In_ REFIID Riid,
    _In_opt_ PROPVARIANT *ActivationParams,
    _In_ IActivateAudioInterfaceCompletionHandler *CompletionHandler,
    _Outptr_ IActivateAudioInterfaceAsyncOperation **ActivationOperation)
{
    PMMD_ACTIVATION Activation;
    IUnknown *Agile;
    SIZE_T Bytes;
    HRESULT Hr;

    if (!DeviceInterfacePath || !CompletionHandler)
        return E_INVALIDARG;
    if (!ActivationOperation)
        return E_POINTER;
    *ActivationOperation = NULL;

    /* The handler is called on another thread, so it has to say it may be */
    if (FAILED(CompletionHandler->QueryInterface(MmdIidAgileObject, (VOID **)&Agile)))
        return E_ILLEGAL_METHOD_CALL;
    Agile->Release();

    Activation = (PMMD_ACTIVATION)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Activation));
    if (!Activation)
        return E_OUTOFMEMORY;
    Bytes = (wcslen(DeviceInterfacePath) + 1) * sizeof(WCHAR);
    Activation->Path = (PWSTR)HeapAlloc(GetProcessHeap(), 0, Bytes);
    Activation->Operation = new (std::nothrow) CActivateOperation();
    PropVariantInit(&Activation->Parameters);
    Hr = (Activation->Path && Activation->Operation) ? S_OK : E_OUTOFMEMORY;
    if (SUCCEEDED(Hr) && ActivationParams)
        Hr = PropVariantCopy(&Activation->Parameters, ActivationParams);
    if (FAILED(Hr))
        goto Fail;

    CopyMemory(Activation->Path, DeviceInterfacePath, Bytes);
    Activation->Riid = Riid;
    Activation->Handler = CompletionHandler;
    CompletionHandler->AddRef();

    *ActivationOperation = Activation->Operation;
    Activation->Operation->AddRef();
    if (!QueueUserWorkItem(MmdActivationWorker, Activation, WT_EXECUTEDEFAULT))
    {
        Hr = HRESULT_FROM_WIN32(GetLastError());
        CompletionHandler->Release();
        (*ActivationOperation)->Release();
        *ActivationOperation = NULL;
        goto Fail;
    }
    return S_OK;

Fail:
    if (Activation->Operation)
        Activation->Operation->Release();
    if (Activation->Path)
        HeapFree(GetProcessHeap(), 0, Activation->Path);
    PropVariantClear(&Activation->Parameters);
    HeapFree(GetProcessHeap(), 0, Activation);
    return Hr;
}
