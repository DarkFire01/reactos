/*
 * PROJECT:     ReactOS OpenCL ICD Loader
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private header
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifndef _OPENCL_PRECOMP_H_
#define _OPENCL_PRECOMP_H_

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_NO_STATUS
#define COBJMACROS
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winnls.h>
#include <objbase.h>
#include <cfgmgr32.h>
#include <dxgi.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#define NTOS_MODE_USER
#include <ndk/umtypes.h>
#include <d3dkmthk.h>

#define CL_TARGET_OPENCL_VERSION 300
#define CL_USE_DEPRECATED_OPENCL_1_0_APIS
#define CL_USE_DEPRECATED_OPENCL_1_1_APIS
#define CL_USE_DEPRECATED_OPENCL_1_2_APIS
#define CL_USE_DEPRECATED_OPENCL_2_0_APIS
#define CL_USE_DEPRECATED_OPENCL_2_1_APIS
#define CL_USE_DEPRECATED_OPENCL_2_2_APIS
#include <CL/cl.h>
#include <CL/cl_ext.h>
#include <CL/cl_gl.h>
#include <CL/cl_egl.h>
#include <CL/cl_icd.h>
#include <CL/cl_layer.h>

/* Every object handed out by an ICD starts with its dispatch table (cl_khr_icd) */
struct _cl_platform_id { cl_icd_dispatch *dispatch; };
struct _cl_device_id { cl_icd_dispatch *dispatch; };
struct _cl_context { cl_icd_dispatch *dispatch; };
struct _cl_command_queue { cl_icd_dispatch *dispatch; };
struct _cl_mem { cl_icd_dispatch *dispatch; };
struct _cl_program { cl_icd_dispatch *dispatch; };
struct _cl_kernel { cl_icd_dispatch *dispatch; };
struct _cl_event { cl_icd_dispatch *dispatch; };
struct _cl_sampler { cl_icd_dispatch *dispatch; };

/* One platform exposed by an ICD */
typedef struct _OCL_VENDOR
{
    struct _OCL_VENDOR *Next;
    HMODULE Library;
    PSTR Suffix;
    clGetExtensionFunctionAddress_fn GetExtensionFunctionAddress;
    cl_platform_id Platform;
} OCL_VENDOR, *POCL_VENDOR;

/* One installed layer, Dispatch is what the layer above it calls */
typedef struct _OCL_LAYER
{
    struct _OCL_LAYER *Next;
    HMODULE Library;
    cl_icd_dispatch Dispatch;
} OCL_LAYER, *POCL_LAYER;

/* Library name found during discovery, ordered later against DXGI */
typedef struct _OCL_ADAPTER
{
    PSTR LibraryName;
    LUID Luid;
} OCL_ADAPTER, *POCL_ADAPTER;

extern POCL_VENDOR OclpVendorList;
extern POCL_LAYER OclpLayerList;
extern BOOLEAN OclpTraceEnabled;
extern const cl_icd_dispatch OclpLoaderDispatch;
extern const cl_icd_dispatch *OclpTopDispatch;

/* Diagnostics on stderr, enabled by OCL_ICD_ENABLE_TRACE */
#define OclpTrace(...) \
    do { if (OclpTraceEnabled) OclpTraceWrite(__FILE__, __LINE__, __VA_ARGS__); } while (0)

#define OclpTraceW(...) \
    do { if (OclpTraceEnabled) OclpTraceWriteW(__FILE__, __LINE__, __VA_ARGS__); } while (0)

/**
 * @brief Stores Status in ErrorCode when present, for entry points returning a handle.
 *
 * @return NULL, so callers can return it directly.
 */
FORCEINLINE
PVOID
OclpFailHandle(
    _Out_opt_ cl_int *ErrorCode,
    _In_ cl_int Status)
{
    if (ErrorCode)
        *ErrorCode = Status;

    return NULL;
}

/* opencl.c */
VOID
OclpInitialize(VOID);

PVOID
OclpAllocate(
    _In_ SIZE_T Size);

VOID
OclpFree(
    _In_opt_ PVOID Buffer);

VOID
OclpAddVendor(
    _In_opt_z_ PCSTR LibraryName);

VOID
OclpAddLayer(
    _In_opt_z_ PCSTR LibraryName);

VOID
OclpEnumerateEnvironment(VOID);

VOID
OclpEnumerateEnvironmentLayers(VOID);

cl_platform_id
OclpPlatformFromProperties(
    _In_opt_ const cl_context_properties* properties);

cl_int
CL_API_CALL
OclpTermGetPlatformIDs(
    _In_ cl_uint num_entries,
    _Out_writes_opt_(num_entries) cl_platform_id* platforms,
    _Out_opt_ cl_uint* num_platforms);

void*
CL_API_CALL
OclpTermGetExtensionFunctionAddress(
    _In_z_ const char* func_name);

void*
CL_API_CALL
OclpTermGetExtensionFunctionAddressForPlatform(
    _In_ cl_platform_id platform,
    _In_z_ const char* func_name);

/* icdenum.c */
VOID
OclpTraceWrite(
    _In_z_ PCSTR File,
    _In_ INT Line,
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...);

VOID
OclpTraceWriteW(
    _In_z_ PCSTR File,
    _In_ INT Line,
    _In_z_ _Printf_format_string_ PCWSTR Format,
    ...);

PSTR
OclpGetEnvironment(
    _In_z_ PCSTR Name);

PSTR
OclpGetSecureEnvironment(
    _In_z_ PCSTR Name);

HMODULE
OclpLoadLibrary(
    _In_z_ PCSTR LibraryName);

BOOL
CALLBACK
OclpEnumerateIcds(
    _Inout_ PINIT_ONCE InitOnce,
    _Inout_opt_ PVOID Parameter,
    _Outptr_opt_result_maybenull_ PVOID *Context);

/* extfunc.c */
cl_int
CL_API_CALL
OclpTermGetGLContextInfoKHR(
    _In_opt_ const cl_context_properties* properties,
    _In_ cl_gl_context_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret);

cl_int
CL_API_CALL
OclpTermGetDeviceIDsFromD3D10KHR(
    _In_ cl_platform_id platform,
    _In_ cl_d3d10_device_source_khr d3d_device_source,
    _In_ void* d3d_object,
    _In_ cl_d3d10_device_set_khr d3d_device_set,
    _In_ cl_uint num_entries,
    _Out_writes_opt_(num_entries) cl_device_id* devices,
    _Out_opt_ cl_uint* num_devices);

cl_mem
CL_API_CALL
OclpTermCreateFromD3D10BufferKHR(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ ID3D10Buffer* resource,
    _Out_opt_ cl_int* errcode_ret);

cl_mem
CL_API_CALL
OclpTermCreateFromD3D10Texture2DKHR(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ ID3D10Texture2D* resource,
    _In_ UINT subresource,
    _Out_opt_ cl_int* errcode_ret);

cl_mem
CL_API_CALL
OclpTermCreateFromD3D10Texture3DKHR(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ ID3D10Texture3D* resource,
    _In_ UINT subresource,
    _Out_opt_ cl_int* errcode_ret);

cl_int
CL_API_CALL
OclpTermEnqueueAcquireD3D10ObjectsKHR(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_objects,
    _In_reads_(num_objects) const cl_mem* mem_objects,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event);

cl_int
CL_API_CALL
OclpTermEnqueueReleaseD3D10ObjectsKHR(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_objects,
    _In_reads_(num_objects) const cl_mem* mem_objects,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event);

cl_int
CL_API_CALL
OclpTermCreateSubDevicesEXT(
    _In_ cl_device_id in_device,
    _In_ const cl_device_partition_property_ext* properties,
    _In_ cl_uint num_entries,
    _Out_writes_opt_(num_entries) cl_device_id* out_devices,
    _In_ cl_uint* num_devices);

cl_int
CL_API_CALL
OclpTermRetainDeviceEXT(
    _In_ cl_device_id device);

cl_int
CL_API_CALL
OclpTermReleaseDeviceEXT(
    _In_ cl_device_id device);

cl_event
CL_API_CALL
OclpTermCreateEventFromGLsyncKHR(
    _In_ cl_context context,
    _In_ cl_GLsync sync,
    _Out_opt_ cl_int* errcode_ret);

cl_int
CL_API_CALL
OclpTermGetDeviceIDsFromD3D11KHR(
    _In_ cl_platform_id platform,
    _In_ cl_d3d11_device_source_khr d3d_device_source,
    _In_ void* d3d_object,
    _In_ cl_d3d11_device_set_khr d3d_device_set,
    _In_ cl_uint num_entries,
    _Out_writes_opt_(num_entries) cl_device_id* devices,
    _Out_opt_ cl_uint* num_devices);

cl_mem
CL_API_CALL
OclpTermCreateFromD3D11BufferKHR(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ ID3D11Buffer* resource,
    _Out_opt_ cl_int* errcode_ret);

cl_mem
CL_API_CALL
OclpTermCreateFromD3D11Texture2DKHR(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ ID3D11Texture2D* resource,
    _In_ UINT subresource,
    _Out_opt_ cl_int* errcode_ret);

cl_mem
CL_API_CALL
OclpTermCreateFromD3D11Texture3DKHR(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ ID3D11Texture3D* resource,
    _In_ UINT subresource,
    _Out_opt_ cl_int* errcode_ret);

cl_mem
CL_API_CALL
OclpTermCreateFromDX9MediaSurfaceKHR(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ cl_dx9_media_adapter_type_khr adapter_type,
    _In_ void* surface_info,
    _In_ cl_uint plane,
    _Out_opt_ cl_int* errcode_ret);

cl_int
CL_API_CALL
OclpTermEnqueueAcquireD3D11ObjectsKHR(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_objects,
    _In_reads_(num_objects) const cl_mem* mem_objects,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event);

cl_int
CL_API_CALL
OclpTermEnqueueReleaseD3D11ObjectsKHR(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_objects,
    _In_reads_(num_objects) const cl_mem* mem_objects,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event);

cl_int
CL_API_CALL
OclpTermGetDeviceIDsFromDX9MediaAdapterKHR(
    _In_ cl_platform_id platform,
    _In_ cl_uint num_media_adapters,
    _In_reads_(num_media_adapters) cl_dx9_media_adapter_type_khr* media_adapter_type,
    _In_reads_(num_media_adapters) void* media_adapters,
    _In_ cl_dx9_media_adapter_set_khr media_adapter_set,
    _In_ cl_uint num_entries,
    _Out_writes_opt_(num_entries) cl_device_id* devices,
    _Out_opt_ cl_uint* num_devices);

cl_int
CL_API_CALL
OclpTermEnqueueAcquireDX9MediaSurfacesKHR(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_objects,
    _In_reads_(num_objects) const cl_mem* mem_objects,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event);

cl_int
CL_API_CALL
OclpTermEnqueueReleaseDX9MediaSurfacesKHR(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_objects,
    _In_reads_(num_objects) const cl_mem* mem_objects,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event);

cl_mem
CL_API_CALL
OclpTermCreateFromEGLImageKHR(
    _In_ cl_context context,
    _In_ CLeglDisplayKHR egldisplay,
    _In_ CLeglImageKHR eglimage,
    _In_ cl_mem_flags flags,
    _In_opt_ const cl_egl_image_properties_khr* properties,
    _Out_opt_ cl_int* errcode_ret);

cl_int
CL_API_CALL
OclpTermEnqueueAcquireEGLObjectsKHR(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_objects,
    _In_reads_(num_objects) const cl_mem* mem_objects,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event);

cl_int
CL_API_CALL
OclpTermEnqueueReleaseEGLObjectsKHR(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_objects,
    _In_reads_(num_objects) const cl_mem* mem_objects,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event);

cl_event
CL_API_CALL
OclpTermCreateEventFromEGLSyncKHR(
    _In_ cl_context context,
    _In_ CLeglSyncKHR sync,
    _In_ CLeglDisplayKHR display,
    _Out_opt_ cl_int* errcode_ret);

cl_int
CL_API_CALL
OclpTermGetKernelSubGroupInfoKHR(
    _In_ cl_kernel in_kernel,
    _In_ cl_device_id in_device,
    _In_ cl_kernel_sub_group_info param_name,
    _In_ size_t input_value_size,
    _In_reads_bytes_opt_(input_value_size) const void* input_value,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret);

#endif /* _OPENCL_PRECOMP_H_ */
