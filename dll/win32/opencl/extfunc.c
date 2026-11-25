/*
 * PROJECT:     ReactOS OpenCL ICD Loader
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Extension entry points known to the loader
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "precomp.h"

/* FUNCTIONS ******************************************************************/

cl_int
CL_API_CALL
OclpTermGetGLContextInfoKHR(
    _In_opt_ const cl_context_properties* properties,
    _In_ cl_gl_context_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    cl_platform_id Platform;

    Platform = OclpPlatformFromProperties(properties);
    if (!Platform)
        return CL_INVALID_PLATFORM;
    if (!Platform->dispatch->clGetGLContextInfoKHR)
        return CL_INVALID_OPERATION;

    return Platform->dispatch->clGetGLContextInfoKHR(properties,
                                                     param_name,
                                                     param_value_size,
                                                     param_value,
                                                     param_value_size_ret);
}

cl_int
CL_API_CALL
clGetGLContextInfoKHR(
    _In_opt_ const cl_context_properties* properties,
    _In_ cl_gl_context_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    return OclpTopDispatch->clGetGLContextInfoKHR(properties,
                                                  param_name,
                                                  param_value_size,
                                                  param_value,
                                                  param_value_size_ret);
}

cl_int
CL_API_CALL
OclpTermGetDeviceIDsFromD3D10KHR(
    _In_ cl_platform_id platform,
    _In_ cl_d3d10_device_source_khr d3d_device_source,
    _In_ void* d3d_object,
    _In_ cl_d3d10_device_set_khr d3d_device_set,
    _In_ cl_uint num_entries,
    _Out_writes_opt_(num_entries) cl_device_id* devices,
    _Out_opt_ cl_uint* num_devices)
{
    if (!platform)
        return CL_INVALID_PLATFORM;
    if (!platform->dispatch->clGetDeviceIDsFromD3D10KHR)
        return CL_INVALID_OPERATION;

    return platform->dispatch->clGetDeviceIDsFromD3D10KHR(platform,
                                                          d3d_device_source,
                                                          d3d_object,
                                                          d3d_device_set,
                                                          num_entries,
                                                          devices,
                                                          num_devices);
}

cl_int
CL_API_CALL
clGetDeviceIDsFromD3D10KHR(
    _In_ cl_platform_id platform,
    _In_ cl_d3d10_device_source_khr d3d_device_source,
    _In_ void* d3d_object,
    _In_ cl_d3d10_device_set_khr d3d_device_set,
    _In_ cl_uint num_entries,
    _Out_writes_opt_(num_entries) cl_device_id* devices,
    _Out_opt_ cl_uint* num_devices)
{
    return OclpTopDispatch->clGetDeviceIDsFromD3D10KHR(platform,
                                                       d3d_device_source,
                                                       d3d_object,
                                                       d3d_device_set,
                                                       num_entries,
                                                       devices,
                                                       num_devices);
}

cl_mem
CL_API_CALL
OclpTermCreateFromD3D10BufferKHR(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ ID3D10Buffer* resource,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);
    if (!context->dispatch->clCreateFromD3D10BufferKHR)
        return OclpFailHandle(errcode_ret, CL_INVALID_OPERATION);

    return context->dispatch->clCreateFromD3D10BufferKHR(context, flags, resource, errcode_ret);
}

cl_mem
CL_API_CALL
clCreateFromD3D10BufferKHR(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ ID3D10Buffer* resource,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateFromD3D10BufferKHR(context, flags, resource, errcode_ret);
}

cl_mem
CL_API_CALL
OclpTermCreateFromD3D10Texture2DKHR(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ ID3D10Texture2D* resource,
    _In_ UINT subresource,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);
    if (!context->dispatch->clCreateFromD3D10Texture2DKHR)
        return OclpFailHandle(errcode_ret, CL_INVALID_OPERATION);

    return context->dispatch->clCreateFromD3D10Texture2DKHR(context,
                                                            flags,
                                                            resource,
                                                            subresource,
                                                            errcode_ret);
}

cl_mem
CL_API_CALL
clCreateFromD3D10Texture2DKHR(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ ID3D10Texture2D* resource,
    _In_ UINT subresource,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateFromD3D10Texture2DKHR(context,
                                                          flags,
                                                          resource,
                                                          subresource,
                                                          errcode_ret);
}

cl_mem
CL_API_CALL
OclpTermCreateFromD3D10Texture3DKHR(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ ID3D10Texture3D* resource,
    _In_ UINT subresource,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);
    if (!context->dispatch->clCreateFromD3D10Texture3DKHR)
        return OclpFailHandle(errcode_ret, CL_INVALID_OPERATION);

    return context->dispatch->clCreateFromD3D10Texture3DKHR(context,
                                                            flags,
                                                            resource,
                                                            subresource,
                                                            errcode_ret);
}

cl_mem
CL_API_CALL
clCreateFromD3D10Texture3DKHR(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ ID3D10Texture3D* resource,
    _In_ UINT subresource,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateFromD3D10Texture3DKHR(context,
                                                          flags,
                                                          resource,
                                                          subresource,
                                                          errcode_ret);
}

cl_int
CL_API_CALL
OclpTermEnqueueAcquireD3D10ObjectsKHR(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_objects,
    _In_reads_(num_objects) const cl_mem* mem_objects,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;
    if (!command_queue->dispatch->clEnqueueAcquireD3D10ObjectsKHR)
        return CL_INVALID_OPERATION;

    return command_queue->dispatch->clEnqueueAcquireD3D10ObjectsKHR(command_queue,
                                                                    num_objects,
                                                                    mem_objects,
                                                                    num_events_in_wait_list,
                                                                    event_wait_list,
                                                                    event);
}

cl_int
CL_API_CALL
clEnqueueAcquireD3D10ObjectsKHR(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_objects,
    _In_reads_(num_objects) const cl_mem* mem_objects,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueAcquireD3D10ObjectsKHR(command_queue,
                                                            num_objects,
                                                            mem_objects,
                                                            num_events_in_wait_list,
                                                            event_wait_list,
                                                            event);
}

cl_int
CL_API_CALL
OclpTermEnqueueReleaseD3D10ObjectsKHR(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_objects,
    _In_reads_(num_objects) const cl_mem* mem_objects,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;
    if (!command_queue->dispatch->clEnqueueReleaseD3D10ObjectsKHR)
        return CL_INVALID_OPERATION;

    return command_queue->dispatch->clEnqueueReleaseD3D10ObjectsKHR(command_queue,
                                                                    num_objects,
                                                                    mem_objects,
                                                                    num_events_in_wait_list,
                                                                    event_wait_list,
                                                                    event);
}

cl_int
CL_API_CALL
clEnqueueReleaseD3D10ObjectsKHR(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_objects,
    _In_reads_(num_objects) const cl_mem* mem_objects,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueReleaseD3D10ObjectsKHR(command_queue,
                                                            num_objects,
                                                            mem_objects,
                                                            num_events_in_wait_list,
                                                            event_wait_list,
                                                            event);
}

cl_int
CL_API_CALL
OclpTermCreateSubDevicesEXT(
    _In_ cl_device_id in_device,
    _In_ const cl_device_partition_property_ext* properties,
    _In_ cl_uint num_entries,
    _Out_writes_opt_(num_entries) cl_device_id* out_devices,
    _In_ cl_uint* num_devices)
{
    if (!in_device)
        return CL_INVALID_DEVICE;
    if (!in_device->dispatch->clCreateSubDevicesEXT)
        return CL_INVALID_OPERATION;

    return in_device->dispatch->clCreateSubDevicesEXT(in_device,
                                                      properties,
                                                      num_entries,
                                                      out_devices,
                                                      num_devices);
}

cl_int
CL_API_CALL
clCreateSubDevicesEXT(
    _In_ cl_device_id in_device,
    _In_ const cl_device_partition_property_ext* properties,
    _In_ cl_uint num_entries,
    _Out_writes_opt_(num_entries) cl_device_id* out_devices,
    _In_ cl_uint* num_devices)
{
    return OclpTopDispatch->clCreateSubDevicesEXT(in_device,
                                                  properties,
                                                  num_entries,
                                                  out_devices,
                                                  num_devices);
}

cl_int
CL_API_CALL
OclpTermRetainDeviceEXT(
    _In_ cl_device_id device)
{
    if (!device)
        return CL_INVALID_DEVICE;
    if (!device->dispatch->clRetainDeviceEXT)
        return CL_INVALID_OPERATION;

    return device->dispatch->clRetainDeviceEXT(device);
}

cl_int
CL_API_CALL
clRetainDeviceEXT(
    _In_ cl_device_id device)
{
    return OclpTopDispatch->clRetainDeviceEXT(device);
}

cl_int
CL_API_CALL
OclpTermReleaseDeviceEXT(
    _In_ cl_device_id device)
{
    if (!device)
        return CL_INVALID_DEVICE;
    if (!device->dispatch->clReleaseDeviceEXT)
        return CL_INVALID_OPERATION;

    return device->dispatch->clReleaseDeviceEXT(device);
}

cl_int
CL_API_CALL
clReleaseDeviceEXT(
    _In_ cl_device_id device)
{
    return OclpTopDispatch->clReleaseDeviceEXT(device);
}

cl_event
CL_API_CALL
OclpTermCreateEventFromGLsyncKHR(
    _In_ cl_context context,
    _In_ cl_GLsync sync,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);
    if (!context->dispatch->clCreateEventFromGLsyncKHR)
        return OclpFailHandle(errcode_ret, CL_INVALID_OPERATION);

    return context->dispatch->clCreateEventFromGLsyncKHR(context, sync, errcode_ret);
}

cl_event
CL_API_CALL
clCreateEventFromGLsyncKHR(
    _In_ cl_context context,
    _In_ cl_GLsync sync,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateEventFromGLsyncKHR(context, sync, errcode_ret);
}

cl_int
CL_API_CALL
OclpTermGetDeviceIDsFromD3D11KHR(
    _In_ cl_platform_id platform,
    _In_ cl_d3d11_device_source_khr d3d_device_source,
    _In_ void* d3d_object,
    _In_ cl_d3d11_device_set_khr d3d_device_set,
    _In_ cl_uint num_entries,
    _Out_writes_opt_(num_entries) cl_device_id* devices,
    _Out_opt_ cl_uint* num_devices)
{
    if (!platform)
        return CL_INVALID_PLATFORM;
    if (!platform->dispatch->clGetDeviceIDsFromD3D11KHR)
        return CL_INVALID_OPERATION;

    return platform->dispatch->clGetDeviceIDsFromD3D11KHR(platform,
                                                          d3d_device_source,
                                                          d3d_object,
                                                          d3d_device_set,
                                                          num_entries,
                                                          devices,
                                                          num_devices);
}

cl_int
CL_API_CALL
clGetDeviceIDsFromD3D11KHR(
    _In_ cl_platform_id platform,
    _In_ cl_d3d11_device_source_khr d3d_device_source,
    _In_ void* d3d_object,
    _In_ cl_d3d11_device_set_khr d3d_device_set,
    _In_ cl_uint num_entries,
    _Out_writes_opt_(num_entries) cl_device_id* devices,
    _Out_opt_ cl_uint* num_devices)
{
    return OclpTopDispatch->clGetDeviceIDsFromD3D11KHR(platform,
                                                       d3d_device_source,
                                                       d3d_object,
                                                       d3d_device_set,
                                                       num_entries,
                                                       devices,
                                                       num_devices);
}

cl_mem
CL_API_CALL
OclpTermCreateFromD3D11BufferKHR(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ ID3D11Buffer* resource,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);
    if (!context->dispatch->clCreateFromD3D11BufferKHR)
        return OclpFailHandle(errcode_ret, CL_INVALID_OPERATION);

    return context->dispatch->clCreateFromD3D11BufferKHR(context, flags, resource, errcode_ret);
}

cl_mem
CL_API_CALL
clCreateFromD3D11BufferKHR(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ ID3D11Buffer* resource,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateFromD3D11BufferKHR(context, flags, resource, errcode_ret);
}

cl_mem
CL_API_CALL
OclpTermCreateFromD3D11Texture2DKHR(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ ID3D11Texture2D* resource,
    _In_ UINT subresource,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);
    if (!context->dispatch->clCreateFromD3D11Texture2DKHR)
        return OclpFailHandle(errcode_ret, CL_INVALID_OPERATION);

    return context->dispatch->clCreateFromD3D11Texture2DKHR(context,
                                                            flags,
                                                            resource,
                                                            subresource,
                                                            errcode_ret);
}

cl_mem
CL_API_CALL
clCreateFromD3D11Texture2DKHR(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ ID3D11Texture2D* resource,
    _In_ UINT subresource,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateFromD3D11Texture2DKHR(context,
                                                          flags,
                                                          resource,
                                                          subresource,
                                                          errcode_ret);
}

cl_mem
CL_API_CALL
OclpTermCreateFromD3D11Texture3DKHR(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ ID3D11Texture3D* resource,
    _In_ UINT subresource,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);
    if (!context->dispatch->clCreateFromD3D11Texture3DKHR)
        return OclpFailHandle(errcode_ret, CL_INVALID_OPERATION);

    return context->dispatch->clCreateFromD3D11Texture3DKHR(context,
                                                            flags,
                                                            resource,
                                                            subresource,
                                                            errcode_ret);
}

cl_mem
CL_API_CALL
clCreateFromD3D11Texture3DKHR(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ ID3D11Texture3D* resource,
    _In_ UINT subresource,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateFromD3D11Texture3DKHR(context,
                                                          flags,
                                                          resource,
                                                          subresource,
                                                          errcode_ret);
}

cl_mem
CL_API_CALL
OclpTermCreateFromDX9MediaSurfaceKHR(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ cl_dx9_media_adapter_type_khr adapter_type,
    _In_ void* surface_info,
    _In_ cl_uint plane,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);
    if (!context->dispatch->clCreateFromDX9MediaSurfaceKHR)
        return OclpFailHandle(errcode_ret, CL_INVALID_OPERATION);

    return context->dispatch->clCreateFromDX9MediaSurfaceKHR(context,
                                                             flags,
                                                             adapter_type,
                                                             surface_info,
                                                             plane,
                                                             errcode_ret);
}

cl_mem
CL_API_CALL
clCreateFromDX9MediaSurfaceKHR(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ cl_dx9_media_adapter_type_khr adapter_type,
    _In_ void* surface_info,
    _In_ cl_uint plane,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateFromDX9MediaSurfaceKHR(context,
                                                           flags,
                                                           adapter_type,
                                                           surface_info,
                                                           plane,
                                                           errcode_ret);
}

cl_int
CL_API_CALL
OclpTermEnqueueAcquireD3D11ObjectsKHR(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_objects,
    _In_reads_(num_objects) const cl_mem* mem_objects,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;
    if (!command_queue->dispatch->clEnqueueAcquireD3D11ObjectsKHR)
        return CL_INVALID_OPERATION;

    return command_queue->dispatch->clEnqueueAcquireD3D11ObjectsKHR(command_queue,
                                                                    num_objects,
                                                                    mem_objects,
                                                                    num_events_in_wait_list,
                                                                    event_wait_list,
                                                                    event);
}

cl_int
CL_API_CALL
clEnqueueAcquireD3D11ObjectsKHR(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_objects,
    _In_reads_(num_objects) const cl_mem* mem_objects,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueAcquireD3D11ObjectsKHR(command_queue,
                                                            num_objects,
                                                            mem_objects,
                                                            num_events_in_wait_list,
                                                            event_wait_list,
                                                            event);
}

cl_int
CL_API_CALL
OclpTermEnqueueReleaseD3D11ObjectsKHR(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_objects,
    _In_reads_(num_objects) const cl_mem* mem_objects,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;
    if (!command_queue->dispatch->clEnqueueReleaseD3D11ObjectsKHR)
        return CL_INVALID_OPERATION;

    return command_queue->dispatch->clEnqueueReleaseD3D11ObjectsKHR(command_queue,
                                                                    num_objects,
                                                                    mem_objects,
                                                                    num_events_in_wait_list,
                                                                    event_wait_list,
                                                                    event);
}

cl_int
CL_API_CALL
clEnqueueReleaseD3D11ObjectsKHR(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_objects,
    _In_reads_(num_objects) const cl_mem* mem_objects,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueReleaseD3D11ObjectsKHR(command_queue,
                                                            num_objects,
                                                            mem_objects,
                                                            num_events_in_wait_list,
                                                            event_wait_list,
                                                            event);
}

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
    _Out_opt_ cl_uint* num_devices)
{
    if (!platform)
        return CL_INVALID_PLATFORM;
    if (!platform->dispatch->clGetDeviceIDsFromDX9MediaAdapterKHR)
        return CL_INVALID_OPERATION;

    return platform->dispatch->clGetDeviceIDsFromDX9MediaAdapterKHR(platform,
                                                                    num_media_adapters,
                                                                    media_adapter_type,
                                                                    media_adapters,
                                                                    media_adapter_set,
                                                                    num_entries,
                                                                    devices,
                                                                    num_devices);
}

cl_int
CL_API_CALL
clGetDeviceIDsFromDX9MediaAdapterKHR(
    _In_ cl_platform_id platform,
    _In_ cl_uint num_media_adapters,
    _In_reads_(num_media_adapters) cl_dx9_media_adapter_type_khr* media_adapter_type,
    _In_reads_(num_media_adapters) void* media_adapters,
    _In_ cl_dx9_media_adapter_set_khr media_adapter_set,
    _In_ cl_uint num_entries,
    _Out_writes_opt_(num_entries) cl_device_id* devices,
    _Out_opt_ cl_uint* num_devices)
{
    return OclpTopDispatch->clGetDeviceIDsFromDX9MediaAdapterKHR(platform,
                                                                 num_media_adapters,
                                                                 media_adapter_type,
                                                                 media_adapters,
                                                                 media_adapter_set,
                                                                 num_entries,
                                                                 devices,
                                                                 num_devices);
}

cl_int
CL_API_CALL
OclpTermEnqueueAcquireDX9MediaSurfacesKHR(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_objects,
    _In_reads_(num_objects) const cl_mem* mem_objects,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;
    if (!command_queue->dispatch->clEnqueueAcquireDX9MediaSurfacesKHR)
        return CL_INVALID_OPERATION;

    return command_queue->dispatch->clEnqueueAcquireDX9MediaSurfacesKHR(command_queue,
                                                                        num_objects,
                                                                        mem_objects,
                                                                        num_events_in_wait_list,
                                                                        event_wait_list,
                                                                        event);
}

cl_int
CL_API_CALL
clEnqueueAcquireDX9MediaSurfacesKHR(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_objects,
    _In_reads_(num_objects) const cl_mem* mem_objects,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueAcquireDX9MediaSurfacesKHR(command_queue,
                                                                num_objects,
                                                                mem_objects,
                                                                num_events_in_wait_list,
                                                                event_wait_list,
                                                                event);
}

cl_int
CL_API_CALL
OclpTermEnqueueReleaseDX9MediaSurfacesKHR(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_objects,
    _In_reads_(num_objects) const cl_mem* mem_objects,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;
    if (!command_queue->dispatch->clEnqueueReleaseDX9MediaSurfacesKHR)
        return CL_INVALID_OPERATION;

    return command_queue->dispatch->clEnqueueReleaseDX9MediaSurfacesKHR(command_queue,
                                                                        num_objects,
                                                                        mem_objects,
                                                                        num_events_in_wait_list,
                                                                        event_wait_list,
                                                                        event);
}

cl_int
CL_API_CALL
clEnqueueReleaseDX9MediaSurfacesKHR(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_objects,
    _In_reads_(num_objects) const cl_mem* mem_objects,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueReleaseDX9MediaSurfacesKHR(command_queue,
                                                                num_objects,
                                                                mem_objects,
                                                                num_events_in_wait_list,
                                                                event_wait_list,
                                                                event);
}

cl_mem
CL_API_CALL
OclpTermCreateFromEGLImageKHR(
    _In_ cl_context context,
    _In_ CLeglDisplayKHR egldisplay,
    _In_ CLeglImageKHR eglimage,
    _In_ cl_mem_flags flags,
    _In_opt_ const cl_egl_image_properties_khr* properties,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);
    if (!context->dispatch->clCreateFromEGLImageKHR)
        return OclpFailHandle(errcode_ret, CL_INVALID_OPERATION);

    return context->dispatch->clCreateFromEGLImageKHR(context,
                                                      egldisplay,
                                                      eglimage,
                                                      flags,
                                                      properties,
                                                      errcode_ret);
}

cl_mem
CL_API_CALL
clCreateFromEGLImageKHR(
    _In_ cl_context context,
    _In_ CLeglDisplayKHR egldisplay,
    _In_ CLeglImageKHR eglimage,
    _In_ cl_mem_flags flags,
    _In_opt_ const cl_egl_image_properties_khr* properties,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateFromEGLImageKHR(context,
                                                    egldisplay,
                                                    eglimage,
                                                    flags,
                                                    properties,
                                                    errcode_ret);
}

cl_int
CL_API_CALL
OclpTermEnqueueAcquireEGLObjectsKHR(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_objects,
    _In_reads_(num_objects) const cl_mem* mem_objects,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;
    if (!command_queue->dispatch->clEnqueueAcquireEGLObjectsKHR)
        return CL_INVALID_OPERATION;

    return command_queue->dispatch->clEnqueueAcquireEGLObjectsKHR(command_queue,
                                                                  num_objects,
                                                                  mem_objects,
                                                                  num_events_in_wait_list,
                                                                  event_wait_list,
                                                                  event);
}

cl_int
CL_API_CALL
clEnqueueAcquireEGLObjectsKHR(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_objects,
    _In_reads_(num_objects) const cl_mem* mem_objects,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueAcquireEGLObjectsKHR(command_queue,
                                                          num_objects,
                                                          mem_objects,
                                                          num_events_in_wait_list,
                                                          event_wait_list,
                                                          event);
}

cl_int
CL_API_CALL
OclpTermEnqueueReleaseEGLObjectsKHR(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_objects,
    _In_reads_(num_objects) const cl_mem* mem_objects,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;
    if (!command_queue->dispatch->clEnqueueReleaseEGLObjectsKHR)
        return CL_INVALID_OPERATION;

    return command_queue->dispatch->clEnqueueReleaseEGLObjectsKHR(command_queue,
                                                                  num_objects,
                                                                  mem_objects,
                                                                  num_events_in_wait_list,
                                                                  event_wait_list,
                                                                  event);
}

cl_int
CL_API_CALL
clEnqueueReleaseEGLObjectsKHR(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_objects,
    _In_reads_(num_objects) const cl_mem* mem_objects,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueReleaseEGLObjectsKHR(command_queue,
                                                          num_objects,
                                                          mem_objects,
                                                          num_events_in_wait_list,
                                                          event_wait_list,
                                                          event);
}

cl_event
CL_API_CALL
OclpTermCreateEventFromEGLSyncKHR(
    _In_ cl_context context,
    _In_ CLeglSyncKHR sync,
    _In_ CLeglDisplayKHR display,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);
    if (!context->dispatch->clCreateEventFromEGLSyncKHR)
        return OclpFailHandle(errcode_ret, CL_INVALID_OPERATION);

    return context->dispatch->clCreateEventFromEGLSyncKHR(context, sync, display, errcode_ret);
}

cl_event
CL_API_CALL
clCreateEventFromEGLSyncKHR(
    _In_ cl_context context,
    _In_ CLeglSyncKHR sync,
    _In_ CLeglDisplayKHR display,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateEventFromEGLSyncKHR(context, sync, display, errcode_ret);
}

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
    _Out_opt_ size_t* param_value_size_ret)
{
    if (!in_kernel)
        return CL_INVALID_KERNEL;
    if (!in_kernel->dispatch->clGetKernelSubGroupInfoKHR)
        return CL_INVALID_OPERATION;

    return in_kernel->dispatch->clGetKernelSubGroupInfoKHR(in_kernel,
                                                           in_device,
                                                           param_name,
                                                           input_value_size,
                                                           input_value,
                                                           param_value_size,
                                                           param_value,
                                                           param_value_size_ret);
}

cl_int
CL_API_CALL
clGetKernelSubGroupInfoKHR(
    _In_ cl_kernel in_kernel,
    _In_ cl_device_id in_device,
    _In_ cl_kernel_sub_group_info param_name,
    _In_ size_t input_value_size,
    _In_reads_bytes_opt_(input_value_size) const void* input_value,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    return OclpTopDispatch->clGetKernelSubGroupInfoKHR(in_kernel,
                                                       in_device,
                                                       param_name,
                                                       input_value_size,
                                                       input_value,
                                                       param_value_size,
                                                       param_value,
                                                       param_value_size_ret);
}
