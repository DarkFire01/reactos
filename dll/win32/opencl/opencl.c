/*
 * PROJECT:     ReactOS OpenCL ICD Loader
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Vendor and layer management, platform enumeration
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "precomp.h"

/* GLOBALS ********************************************************************/

POCL_VENDOR OclpVendorList;
POCL_LAYER OclpLayerList;
BOOLEAN OclpTraceEnabled;

static INIT_ONCE OclpInitOnce = INIT_ONCE_STATIC_INIT;

/* Functions the loader resolves itself instead of asking an ICD */
static const struct
{
    PCSTR Name;
    PVOID Address;
} OclpLoaderExtensions[] =
{
    { "clCreateFromGLBuffer", (PVOID)clCreateFromGLBuffer },
    { "clCreateFromGLTexture", (PVOID)clCreateFromGLTexture },
    { "clCreateFromGLTexture2D", (PVOID)clCreateFromGLTexture2D },
    { "clCreateFromGLTexture3D", (PVOID)clCreateFromGLTexture3D },
    { "clCreateFromGLRenderbuffer", (PVOID)clCreateFromGLRenderbuffer },
    { "clGetGLObjectInfo", (PVOID)clGetGLObjectInfo },
    { "clGetGLTextureInfo", (PVOID)clGetGLTextureInfo },
    { "clEnqueueAcquireGLObjects", (PVOID)clEnqueueAcquireGLObjects },
    { "clEnqueueReleaseGLObjects", (PVOID)clEnqueueReleaseGLObjects },
    { "clGetGLContextInfoKHR", (PVOID)clGetGLContextInfoKHR },
    { "clCreateEventFromGLsyncKHR", (PVOID)clCreateEventFromGLsyncKHR },
    { "clGetDeviceIDsFromD3D10KHR", (PVOID)clGetDeviceIDsFromD3D10KHR },
    { "clCreateFromD3D10BufferKHR", (PVOID)clCreateFromD3D10BufferKHR },
    { "clCreateFromD3D10Texture2DKHR", (PVOID)clCreateFromD3D10Texture2DKHR },
    { "clCreateFromD3D10Texture3DKHR", (PVOID)clCreateFromD3D10Texture3DKHR },
    { "clEnqueueAcquireD3D10ObjectsKHR", (PVOID)clEnqueueAcquireD3D10ObjectsKHR },
    { "clEnqueueReleaseD3D10ObjectsKHR", (PVOID)clEnqueueReleaseD3D10ObjectsKHR },
    { "clGetDeviceIDsFromD3D11KHR", (PVOID)clGetDeviceIDsFromD3D11KHR },
    { "clCreateFromD3D11BufferKHR", (PVOID)clCreateFromD3D11BufferKHR },
    { "clCreateFromD3D11Texture2DKHR", (PVOID)clCreateFromD3D11Texture2DKHR },
    { "clCreateFromD3D11Texture3DKHR", (PVOID)clCreateFromD3D11Texture3DKHR },
    { "clEnqueueAcquireD3D11ObjectsKHR", (PVOID)clEnqueueAcquireD3D11ObjectsKHR },
    { "clEnqueueReleaseD3D11ObjectsKHR", (PVOID)clEnqueueReleaseD3D11ObjectsKHR },
    { "clGetDeviceIDsFromDX9MediaAdapterKHR", (PVOID)clGetDeviceIDsFromDX9MediaAdapterKHR },
    { "clCreateFromDX9MediaSurfaceKHR", (PVOID)clCreateFromDX9MediaSurfaceKHR },
    { "clEnqueueAcquireDX9MediaSurfacesKHR", (PVOID)clEnqueueAcquireDX9MediaSurfacesKHR },
    { "clEnqueueReleaseDX9MediaSurfacesKHR", (PVOID)clEnqueueReleaseDX9MediaSurfacesKHR },
    { "clCreateSubDevicesEXT", (PVOID)clCreateSubDevicesEXT },
    { "clRetainDeviceEXT", (PVOID)clRetainDeviceEXT },
    { "clReleaseDeviceEXT", (PVOID)clReleaseDeviceEXT },
    { "clCreateFromEGLImageKHR", (PVOID)clCreateFromEGLImageKHR },
    { "clEnqueueAcquireEGLObjectsKHR", (PVOID)clEnqueueAcquireEGLObjectsKHR },
    { "clEnqueueReleaseEGLObjectsKHR", (PVOID)clEnqueueReleaseEGLObjectsKHR },
    { "clCreateEventFromEGLSyncKHR", (PVOID)clCreateEventFromEGLSyncKHR },
    { "clGetKernelSubGroupInfoKHR", (PVOID)clGetKernelSubGroupInfoKHR },
    { "clGetICDLoaderInfoOCLICD", (PVOID)clGetICDLoaderInfoOCLICD },
};

/* FUNCTIONS ******************************************************************/

BOOL
WINAPI
DllMain(
    _In_ HINSTANCE hinstDLL,
    _In_ DWORD fdwReason,
    _In_ LPVOID lpvReserved)
{
    UNREFERENCED_PARAMETER(hinstDLL);
    UNREFERENCED_PARAMETER(fdwReason);
    UNREFERENCED_PARAMETER(lpvReserved);

    return TRUE;
}

/**
 * @brief Runs ICD discovery once per process.
 *
 * A discovery pass that finds nothing is not latched and runs again on the next call.
 */
VOID
OclpInitialize(VOID)
{
    InitOnceExecuteOnce(&OclpInitOnce, OclpEnumerateIcds, NULL, NULL);
}

PVOID
OclpAllocate(
    _In_ SIZE_T Size)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
}

VOID
OclpFree(
    _In_opt_ PVOID Buffer)
{
    if (Buffer)
        HeapFree(GetProcessHeap(), 0, Buffer);
}

/**
 * @brief Loads an ICD and appends one vendor entry per platform it reports.
 */
VOID
OclpAddVendor(
    _In_opt_z_ PCSTR LibraryName)
{
    clGetExtensionFunctionAddress_fn GetExtensionAddress;
    clIcdGetPlatformIDsKHR_fn GetIcdPlatformIds;
    cl_platform_id *Platforms = NULL;
    cl_uint PlatformCount = 0;
    POCL_VENDOR Vendor, *Tail;
    HMODULE Library;
    size_t SuffixSize;
    cl_int Status;
    PSTR Suffix;
    cl_uint i;

    if (!LibraryName)
        return;

    OclpTrace("attempting to add vendor %s...\n", LibraryName);

    Library = OclpLoadLibrary(LibraryName);
    if (!Library)
    {
        OclpTrace("failed to load library %s\n", LibraryName);
        return;
    }

    for (Vendor = OclpVendorList; Vendor; Vendor = Vendor->Next)
    {
        if (Vendor->Library == Library)
        {
            OclpTrace("already loaded vendor %s, nothing to do here\n", LibraryName);
            goto Cleanup;
        }
    }

    GetExtensionAddress = (clGetExtensionFunctionAddress_fn)GetProcAddress(Library, "clGetExtensionFunctionAddress");
    if (!GetExtensionAddress)
    {
        OclpTrace("failed to get function address clGetExtensionFunctionAddress\n");
        goto Cleanup;
    }

    GetIcdPlatformIds = (clIcdGetPlatformIDsKHR_fn)GetExtensionAddress("clIcdGetPlatformIDsKHR");
    if (!GetIcdPlatformIds)
    {
        OclpTrace("failed to get extension function address clIcdGetPlatformIDsKHR\n");
        goto Cleanup;
    }

    if (GetIcdPlatformIds(0, NULL, &PlatformCount) != CL_SUCCESS)
    {
        OclpTrace("failed clIcdGetPlatformIDs\n");
        goto Cleanup;
    }

    Platforms = OclpAllocate(PlatformCount * sizeof(*Platforms));
    if (!Platforms)
    {
        OclpTrace("failed to allocate memory\n");
        goto Cleanup;
    }

    if (GetIcdPlatformIds(PlatformCount, Platforms, NULL) != CL_SUCCESS)
    {
        OclpTrace("failed clIcdGetPlatformIDs\n");
        goto Cleanup;
    }

    for (i = 0; i < PlatformCount; i++)
    {
        cl_platform_id Platform = Platforms[i];

        if (!Platform)
            continue;

        Status = Platform->dispatch->clGetPlatformInfo(Platform, CL_PLATFORM_ICD_SUFFIX_KHR, 0, NULL, &SuffixSize);
        if (Status != CL_SUCCESS)
            continue;

        Suffix = OclpAllocate(SuffixSize);
        if (!Suffix)
            continue;

        Status = Platform->dispatch->clGetPlatformInfo(Platform, CL_PLATFORM_ICD_SUFFIX_KHR, SuffixSize, Suffix, NULL);
        if (Status != CL_SUCCESS)
        {
            OclpFree(Suffix);
            continue;
        }

        Vendor = OclpAllocate(sizeof(*Vendor));
        if (!Vendor)
        {
            OclpFree(Suffix);
            OclpTrace("failed to allocate memory\n");
            continue;
        }

        /* Every platform keeps its own reference on the ICD */
        Vendor->Library = OclpLoadLibrary(LibraryName);
        if (!Vendor->Library)
        {
            OclpFree(Suffix);
            OclpFree(Vendor);
            OclpTrace("failed get platform handle to library\n");
            continue;
        }

        Vendor->GetExtensionFunctionAddress = GetExtensionAddress;
        Vendor->Platform = Platform;
        Vendor->Suffix = Suffix;

        for (Tail = &OclpVendorList; *Tail; Tail = &(*Tail)->Next);
        *Tail = Vendor;

        OclpTrace("successfully added vendor %s with suffix %s\n", LibraryName, Suffix);
    }

Cleanup:
    FreeLibrary(Library);
    OclpFree(Platforms);
}

/**
 * @brief Loads a layer and puts it on top of the dispatch chain.
 */
VOID
OclpAddLayer(
    _In_opt_z_ PCSTR LibraryName)
{
    const cl_icd_dispatch *LayerTable = NULL;
    const cl_icd_dispatch *Target;
    cl_layer_api_version ApiVersion = 0;
    pfn_clGetLayerInfo GetLayerInfo;
    pfn_clInitLayer InitLayer;
    cl_uint LayerEntries = 0;
    cl_uint LoaderEntries;
    POCL_LAYER Layer = NULL;
    void * const *Source;
    void * const *Below;
    HMODULE Library;
    void **Entries;
    cl_uint i;

    if (!LibraryName)
        return;

    OclpTrace("attempting to add layer %s...\n", LibraryName);

    Library = OclpLoadLibrary(LibraryName);
    if (!Library)
    {
        OclpTrace("failed to load library %s\n", LibraryName);
        return;
    }

    for (Layer = OclpLayerList; Layer; Layer = Layer->Next)
    {
        if (Layer->Library == Library)
        {
            OclpTrace("already loaded layer %s, nothing to do here\n", LibraryName);
            Layer = NULL;
            goto Failure;
        }
    }

    GetLayerInfo = (pfn_clGetLayerInfo)GetProcAddress(Library, "clGetLayerInfo");
    if (!GetLayerInfo)
    {
        OclpTrace("failed to get function address clGetLayerInfo\n");
        goto Failure;
    }

    InitLayer = (pfn_clInitLayer)GetProcAddress(Library, "clInitLayer");
    if (!InitLayer)
    {
        OclpTrace("failed to get function address clInitLayer\n");
        goto Failure;
    }

    if (GetLayerInfo(CL_LAYER_API_VERSION, sizeof(ApiVersion), &ApiVersion, NULL) != CL_SUCCESS)
    {
        OclpTrace("failed to query layer version\n");
        goto Failure;
    }

    if (ApiVersion != CL_LAYER_API_VERSION_100)
    {
        OclpTrace("unsupported api version\n");
        goto Failure;
    }

    Layer = OclpAllocate(sizeof(*Layer));
    if (!Layer)
    {
        OclpTrace("failed to allocate memory\n");
        goto Failure;
    }

    /* The new layer forwards to whatever currently sits on top */
    Target = OclpTopDispatch;
    LoaderEntries = sizeof(cl_icd_dispatch) / sizeof(void *);

    if (InitLayer(LoaderEntries, Target, &LayerEntries, &LayerTable) != CL_SUCCESS)
    {
        OclpTrace("failed to initialize layer\n");
        goto Failure;
    }

    Layer->Library = Library;
    Layer->Next = OclpLayerList;
    OclpLayerList = Layer;

    /* Entries the layer leaves empty fall through to the table below it */
    Entries = (void **)&Layer->Dispatch;
    Source = (void * const *)LayerTable;
    Below = (void * const *)Target;
    for (i = 0; i < LoaderEntries; i++)
        Entries[i] = (i < LayerEntries && Source[i]) ? Source[i] : Below[i];

    OclpTopDispatch = &Layer->Dispatch;

    OclpTrace("successfully added layer %s\n", LibraryName);
    return;

Failure:
    FreeLibrary(Library);
    OclpFree(Layer);
}

/**
 * @brief Calls Callback for every entry of a semicolon separated list, modifying List.
 */
static
VOID
OclpWalkPathList(
    _Inout_z_ PSTR List,
    _In_ VOID (*Callback)(_In_opt_z_ PCSTR LibraryName))
{
    PSTR Current = List;
    PSTR Next;

    while (*Current)
    {
        Next = strchr(Current, ';');
        if (Next)
            *Next++ = ANSI_NULL;
        else
            Next = Current + strlen(Current);

        Callback(Current);
        Current = Next;
    }
}

/**
 * @brief Adds the ICDs listed in OCL_ICD_FILENAMES, ahead of everything the system registers.
 */
VOID
OclpEnumerateEnvironment(VOID)
{
    PSTR FileNames;

    FileNames = OclpGetSecureEnvironment("OCL_ICD_FILENAMES");
    if (!FileNames)
        return;

    OclpTrace("Found OCL_ICD_FILENAMES environment variable.\n");
    OclpWalkPathList(FileNames, OclpAddVendor);
    OclpFree(FileNames);
}

/**
 * @brief Adds the layers listed in OPENCL_LAYERS, the last one ends up on top.
 */
VOID
OclpEnumerateEnvironmentLayers(VOID)
{
    PSTR FileNames;

    FileNames = OclpGetSecureEnvironment("OPENCL_LAYERS");
    if (!FileNames)
        return;

    OclpTrace("Found OPENCL_LAYERS environment variable.\n");
    OclpWalkPathList(FileNames, OclpAddLayer);
    OclpFree(FileNames);
}

/**
 * @brief Picks the platform a context creation call targets.
 *
 * @return The last CL_CONTEXT_PLATFORM value, the first platform when properties is NULL,
 *         or NULL.
 */
cl_platform_id
OclpPlatformFromProperties(
    _In_opt_ const cl_context_properties* properties)
{
    cl_platform_id Platform = NULL;

    if (!properties)
        return OclpVendorList ? OclpVendorList->Platform : NULL;

    for (; properties[0]; properties += 2)
    {
        if (properties[0] == CL_CONTEXT_PLATFORM)
            Platform = (cl_platform_id)properties[1];
    }

    return Platform;
}

static
void*
OclpLookupLoaderExtension(
    _In_z_ const char* func_name)
{
    ULONG i;

    for (i = 0; i < ARRAYSIZE(OclpLoaderExtensions); i++)
    {
        if (!strcmp(OclpLoaderExtensions[i].Name, func_name))
            return OclpLoaderExtensions[i].Address;
    }

    return NULL;
}

cl_int
CL_API_CALL
OclpTermGetPlatformIDs(
    _In_ cl_uint num_entries,
    _Out_writes_opt_(num_entries) cl_platform_id* platforms,
    _Out_opt_ cl_uint* num_platforms)
{
    POCL_VENDOR Vendor;
    cl_uint Count = 0;
    cl_uint i;

    if (platforms && !num_entries)
        return CL_INVALID_VALUE;
    if (!platforms && !num_platforms)
        return CL_INVALID_VALUE;

    if (num_platforms)
        *num_platforms = 0;
    for (i = 0; platforms && i < num_entries; i++)
        platforms[i] = NULL;

    if (!OclpVendorList)
        return CL_PLATFORM_NOT_FOUND_KHR;

    for (Vendor = OclpVendorList; Vendor; Vendor = Vendor->Next)
    {
        if (platforms && Count < num_entries)
            platforms[Count] = Vendor->Platform;
        Count++;
    }

    if (num_platforms)
        *num_platforms = Count;

    return CL_SUCCESS;
}

void*
CL_API_CALL
OclpTermGetExtensionFunctionAddress(
    _In_z_ const char* func_name)
{
    POCL_VENDOR Vendor;
    size_t NameLength;
    size_t SuffixLength;
    void *Address;

    if (!func_name)
        return NULL;

    Address = OclpLookupLoaderExtension(func_name);
    if (Address)
        return Address;

    /* Vendor extensions are routed by their name suffix */
    NameLength = strlen(func_name);
    for (Vendor = OclpVendorList; Vendor; Vendor = Vendor->Next)
    {
        SuffixLength = strlen(Vendor->Suffix);
        if (!SuffixLength || SuffixLength > NameLength)
            continue;

        if (!strcmp(func_name + NameLength - SuffixLength, Vendor->Suffix))
            return Vendor->GetExtensionFunctionAddress(func_name);
    }

    return NULL;
}

void*
CL_API_CALL
OclpTermGetExtensionFunctionAddressForPlatform(
    _In_ cl_platform_id platform,
    _In_z_ const char* func_name)
{
    void *Address;

    if (!func_name)
        return NULL;

    Address = OclpLookupLoaderExtension(func_name);
    if (Address)
        return Address;

    if (!platform)
        return NULL;

    return platform->dispatch->clGetExtensionFunctionAddressForPlatform(platform, func_name);
}

/**
 * @brief cl_loader_info query, reports the Khronos loader version Windows ships.
 */
cl_int
CL_API_CALL
clGetICDLoaderInfoOCLICD(
    _In_ cl_icdl_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    PCSTR Value;
    size_t Size;

    switch (param_name)
    {
        case CL_ICDL_OCL_VERSION:
            Value = "OpenCL 3.0";
            break;

        case CL_ICDL_VERSION:
            Value = "3.0.6";
            break;

        case CL_ICDL_NAME:
            Value = "Khronos OpenCL ICD Loader";
            break;

        case CL_ICDL_VENDOR:
            Value = "Khronos Group";
            break;

        default:
            return CL_INVALID_VALUE;
    }

    Size = strlen(Value) + 1;
    if (param_value)
    {
        if (param_value_size < Size)
            return CL_INVALID_VALUE;

        memcpy(param_value, Value, Size);
    }

    if (param_value_size_ret)
        *param_value_size_ret = Size;

    return CL_SUCCESS;
}
