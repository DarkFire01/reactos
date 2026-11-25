/*
 * PROJECT:     ReactOS OpenCL ICD Loader
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Core OpenCL entry points and the loader dispatch table
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "precomp.h"

/* GLOBALS ********************************************************************/

/* Head of the layer chain, the loader table when no layer is installed */
const cl_icd_dispatch *OclpTopDispatch = &OclpLoaderDispatch;

/* FUNCTIONS ******************************************************************/

cl_int
CL_API_CALL
clGetPlatformIDs(
    _In_ cl_uint num_entries,
    _Out_writes_opt_(num_entries) cl_platform_id* platforms,
    _Out_opt_ cl_uint* num_platforms)
{
    OclpInitialize();

    return OclpTopDispatch->clGetPlatformIDs(num_entries, platforms, num_platforms);
}

static
cl_int
CL_API_CALL
OclpTermGetPlatformInfo(
    _In_ cl_platform_id platform,
    _In_ cl_platform_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    if (!platform)
        return CL_INVALID_PLATFORM;

    return platform->dispatch->clGetPlatformInfo(platform,
                                                 param_name,
                                                 param_value_size,
                                                 param_value,
                                                 param_value_size_ret);
}

cl_int
CL_API_CALL
clGetPlatformInfo(
    _In_ cl_platform_id platform,
    _In_ cl_platform_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    return OclpTopDispatch->clGetPlatformInfo(platform,
                                              param_name,
                                              param_value_size,
                                              param_value,
                                              param_value_size_ret);
}

static
cl_int
CL_API_CALL
OclpTermGetDeviceIDs(
    _In_ cl_platform_id platform,
    _In_ cl_device_type device_type,
    _In_ cl_uint num_entries,
    _Out_writes_opt_(num_entries) cl_device_id* devices,
    _Out_opt_ cl_uint* num_devices)
{
    if (!platform)
        return CL_INVALID_PLATFORM;

    return platform->dispatch->clGetDeviceIDs(platform,
                                              device_type,
                                              num_entries,
                                              devices,
                                              num_devices);
}

cl_int
CL_API_CALL
clGetDeviceIDs(
    _In_ cl_platform_id platform,
    _In_ cl_device_type device_type,
    _In_ cl_uint num_entries,
    _Out_writes_opt_(num_entries) cl_device_id* devices,
    _Out_opt_ cl_uint* num_devices)
{
    return OclpTopDispatch->clGetDeviceIDs(platform,
                                           device_type,
                                           num_entries,
                                           devices,
                                           num_devices);
}

static
cl_int
CL_API_CALL
OclpTermGetDeviceInfo(
    _In_ cl_device_id device,
    _In_ cl_device_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    if (!device)
        return CL_INVALID_DEVICE;

    return device->dispatch->clGetDeviceInfo(device,
                                             param_name,
                                             param_value_size,
                                             param_value,
                                             param_value_size_ret);
}

cl_int
CL_API_CALL
clGetDeviceInfo(
    _In_ cl_device_id device,
    _In_ cl_device_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    return OclpTopDispatch->clGetDeviceInfo(device,
                                            param_name,
                                            param_value_size,
                                            param_value,
                                            param_value_size_ret);
}

static
cl_context
CL_API_CALL
OclpTermCreateContext(
    _In_opt_ const cl_context_properties* properties,
    _In_ cl_uint num_devices,
    _In_reads_(num_devices) const cl_device_id* devices,
    _In_opt_ void (CL_CALLBACK* pfn_notify)(const char* errinfo, const void* private_info, size_t cb, void* user_data),
    _In_opt_ void* user_data,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!num_devices || !devices)
        return OclpFailHandle(errcode_ret, CL_INVALID_VALUE);
    if (!devices[0])
        return OclpFailHandle(errcode_ret, CL_INVALID_DEVICE);

    return devices[0]->dispatch->clCreateContext(properties,
                                                 num_devices,
                                                 devices,
                                                 pfn_notify,
                                                 user_data,
                                                 errcode_ret);
}

cl_context
CL_API_CALL
clCreateContext(
    _In_opt_ const cl_context_properties* properties,
    _In_ cl_uint num_devices,
    _In_reads_(num_devices) const cl_device_id* devices,
    _In_opt_ void (CL_CALLBACK* pfn_notify)(const char* errinfo, const void* private_info, size_t cb, void* user_data),
    _In_opt_ void* user_data,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateContext(properties,
                                            num_devices,
                                            devices,
                                            pfn_notify,
                                            user_data,
                                            errcode_ret);
}

static
cl_context
CL_API_CALL
OclpTermCreateContextFromType(
    _In_opt_ const cl_context_properties* properties,
    _In_ cl_device_type device_type,
    _In_opt_ void (CL_CALLBACK* pfn_notify)(const char* errinfo, const void* private_info, size_t cb, void* user_data),
    _In_opt_ void* user_data,
    _Out_opt_ cl_int* errcode_ret)
{
    cl_platform_id Platform;

    OclpInitialize();

    Platform = OclpPlatformFromProperties(properties);
    if (!Platform)
        return OclpFailHandle(errcode_ret, CL_INVALID_PLATFORM);

    return Platform->dispatch->clCreateContextFromType(properties,
                                                       device_type,
                                                       pfn_notify,
                                                       user_data,
                                                       errcode_ret);
}

cl_context
CL_API_CALL
clCreateContextFromType(
    _In_opt_ const cl_context_properties* properties,
    _In_ cl_device_type device_type,
    _In_opt_ void (CL_CALLBACK* pfn_notify)(const char* errinfo, const void* private_info, size_t cb, void* user_data),
    _In_opt_ void* user_data,
    _Out_opt_ cl_int* errcode_ret)
{
    OclpInitialize();

    return OclpTopDispatch->clCreateContextFromType(properties,
                                                    device_type,
                                                    pfn_notify,
                                                    user_data,
                                                    errcode_ret);
}

static
cl_int
CL_API_CALL
OclpTermRetainContext(
    _In_ cl_context context)
{
    if (!context)
        return CL_INVALID_CONTEXT;

    return context->dispatch->clRetainContext(context);
}

cl_int
CL_API_CALL
clRetainContext(
    _In_ cl_context context)
{
    return OclpTopDispatch->clRetainContext(context);
}

static
cl_int
CL_API_CALL
OclpTermReleaseContext(
    _In_ cl_context context)
{
    if (!context)
        return CL_INVALID_CONTEXT;

    return context->dispatch->clReleaseContext(context);
}

cl_int
CL_API_CALL
clReleaseContext(
    _In_ cl_context context)
{
    return OclpTopDispatch->clReleaseContext(context);
}

static
cl_int
CL_API_CALL
OclpTermGetContextInfo(
    _In_ cl_context context,
    _In_ cl_context_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    if (!context)
        return CL_INVALID_CONTEXT;

    return context->dispatch->clGetContextInfo(context,
                                               param_name,
                                               param_value_size,
                                               param_value,
                                               param_value_size_ret);
}

cl_int
CL_API_CALL
clGetContextInfo(
    _In_ cl_context context,
    _In_ cl_context_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    return OclpTopDispatch->clGetContextInfo(context,
                                             param_name,
                                             param_value_size,
                                             param_value,
                                             param_value_size_ret);
}

static
cl_command_queue
CL_API_CALL
OclpTermCreateCommandQueue(
    _In_ cl_context context,
    _In_ cl_device_id device,
    _In_ cl_command_queue_properties properties,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);

    return context->dispatch->clCreateCommandQueue(context, device, properties, errcode_ret);
}

cl_command_queue
CL_API_CALL
clCreateCommandQueue(
    _In_ cl_context context,
    _In_ cl_device_id device,
    _In_ cl_command_queue_properties properties,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateCommandQueue(context, device, properties, errcode_ret);
}

static
cl_int
CL_API_CALL
OclpTermRetainCommandQueue(
    _In_ cl_command_queue command_queue)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clRetainCommandQueue(command_queue);
}

cl_int
CL_API_CALL
clRetainCommandQueue(
    _In_ cl_command_queue command_queue)
{
    return OclpTopDispatch->clRetainCommandQueue(command_queue);
}

static
cl_int
CL_API_CALL
OclpTermReleaseCommandQueue(
    _In_ cl_command_queue command_queue)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clReleaseCommandQueue(command_queue);
}

cl_int
CL_API_CALL
clReleaseCommandQueue(
    _In_ cl_command_queue command_queue)
{
    return OclpTopDispatch->clReleaseCommandQueue(command_queue);
}

static
cl_int
CL_API_CALL
OclpTermGetCommandQueueInfo(
    _In_ cl_command_queue command_queue,
    _In_ cl_command_queue_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clGetCommandQueueInfo(command_queue,
                                                          param_name,
                                                          param_value_size,
                                                          param_value,
                                                          param_value_size_ret);
}

cl_int
CL_API_CALL
clGetCommandQueueInfo(
    _In_ cl_command_queue command_queue,
    _In_ cl_command_queue_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    return OclpTopDispatch->clGetCommandQueueInfo(command_queue,
                                                  param_name,
                                                  param_value_size,
                                                  param_value,
                                                  param_value_size_ret);
}

static
cl_int
CL_API_CALL
OclpTermSetCommandQueueProperty(
    _In_ cl_command_queue command_queue,
    _In_ cl_command_queue_properties properties,
    _In_ cl_bool enable,
    _Out_opt_ cl_command_queue_properties* old_properties)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clSetCommandQueueProperty(command_queue,
                                                              properties,
                                                              enable,
                                                              old_properties);
}

cl_int
CL_API_CALL
clSetCommandQueueProperty(
    _In_ cl_command_queue command_queue,
    _In_ cl_command_queue_properties properties,
    _In_ cl_bool enable,
    _Out_opt_ cl_command_queue_properties* old_properties)
{
    return OclpTopDispatch->clSetCommandQueueProperty(command_queue,
                                                      properties,
                                                      enable,
                                                      old_properties);
}

static
cl_mem
CL_API_CALL
OclpTermCreateBuffer(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ size_t size,
    _In_opt_ void* host_ptr,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);

    return context->dispatch->clCreateBuffer(context, flags, size, host_ptr, errcode_ret);
}

cl_mem
CL_API_CALL
clCreateBuffer(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ size_t size,
    _In_opt_ void* host_ptr,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateBuffer(context, flags, size, host_ptr, errcode_ret);
}

static
cl_mem
CL_API_CALL
OclpTermCreateImage2D(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ const cl_image_format* image_format,
    _In_ size_t image_width,
    _In_ size_t image_height,
    _In_ size_t image_row_pitch,
    _In_opt_ void* host_ptr,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);

    return context->dispatch->clCreateImage2D(context,
                                              flags,
                                              image_format,
                                              image_width,
                                              image_height,
                                              image_row_pitch,
                                              host_ptr,
                                              errcode_ret);
}

cl_mem
CL_API_CALL
clCreateImage2D(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ const cl_image_format* image_format,
    _In_ size_t image_width,
    _In_ size_t image_height,
    _In_ size_t image_row_pitch,
    _In_opt_ void* host_ptr,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateImage2D(context,
                                            flags,
                                            image_format,
                                            image_width,
                                            image_height,
                                            image_row_pitch,
                                            host_ptr,
                                            errcode_ret);
}

static
cl_mem
CL_API_CALL
OclpTermCreateImage3D(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ const cl_image_format* image_format,
    _In_ size_t image_width,
    _In_ size_t image_height,
    _In_ size_t image_depth,
    _In_ size_t image_row_pitch,
    _In_ size_t image_slice_pitch,
    _In_opt_ void* host_ptr,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);

    return context->dispatch->clCreateImage3D(context,
                                              flags,
                                              image_format,
                                              image_width,
                                              image_height,
                                              image_depth,
                                              image_row_pitch,
                                              image_slice_pitch,
                                              host_ptr,
                                              errcode_ret);
}

cl_mem
CL_API_CALL
clCreateImage3D(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ const cl_image_format* image_format,
    _In_ size_t image_width,
    _In_ size_t image_height,
    _In_ size_t image_depth,
    _In_ size_t image_row_pitch,
    _In_ size_t image_slice_pitch,
    _In_opt_ void* host_ptr,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateImage3D(context,
                                            flags,
                                            image_format,
                                            image_width,
                                            image_height,
                                            image_depth,
                                            image_row_pitch,
                                            image_slice_pitch,
                                            host_ptr,
                                            errcode_ret);
}

static
cl_int
CL_API_CALL
OclpTermRetainMemObject(
    _In_ cl_mem memobj)
{
    if (!memobj)
        return CL_INVALID_MEM_OBJECT;

    return memobj->dispatch->clRetainMemObject(memobj);
}

cl_int
CL_API_CALL
clRetainMemObject(
    _In_ cl_mem memobj)
{
    return OclpTopDispatch->clRetainMemObject(memobj);
}

static
cl_int
CL_API_CALL
OclpTermReleaseMemObject(
    _In_ cl_mem memobj)
{
    if (!memobj)
        return CL_INVALID_MEM_OBJECT;

    return memobj->dispatch->clReleaseMemObject(memobj);
}

cl_int
CL_API_CALL
clReleaseMemObject(
    _In_ cl_mem memobj)
{
    return OclpTopDispatch->clReleaseMemObject(memobj);
}

static
cl_int
CL_API_CALL
OclpTermGetSupportedImageFormats(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ cl_mem_object_type image_type,
    _In_ cl_uint num_entries,
    _Out_writes_opt_(num_entries) cl_image_format* image_formats,
    _Out_opt_ cl_uint* num_image_formats)
{
    if (!context)
        return CL_INVALID_CONTEXT;

    return context->dispatch->clGetSupportedImageFormats(context,
                                                         flags,
                                                         image_type,
                                                         num_entries,
                                                         image_formats,
                                                         num_image_formats);
}

cl_int
CL_API_CALL
clGetSupportedImageFormats(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ cl_mem_object_type image_type,
    _In_ cl_uint num_entries,
    _Out_writes_opt_(num_entries) cl_image_format* image_formats,
    _Out_opt_ cl_uint* num_image_formats)
{
    return OclpTopDispatch->clGetSupportedImageFormats(context,
                                                       flags,
                                                       image_type,
                                                       num_entries,
                                                       image_formats,
                                                       num_image_formats);
}

static
cl_int
CL_API_CALL
OclpTermGetMemObjectInfo(
    _In_ cl_mem memobj,
    _In_ cl_mem_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    if (!memobj)
        return CL_INVALID_MEM_OBJECT;

    return memobj->dispatch->clGetMemObjectInfo(memobj,
                                                param_name,
                                                param_value_size,
                                                param_value,
                                                param_value_size_ret);
}

cl_int
CL_API_CALL
clGetMemObjectInfo(
    _In_ cl_mem memobj,
    _In_ cl_mem_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    return OclpTopDispatch->clGetMemObjectInfo(memobj,
                                               param_name,
                                               param_value_size,
                                               param_value,
                                               param_value_size_ret);
}

static
cl_int
CL_API_CALL
OclpTermGetImageInfo(
    _In_ cl_mem image,
    _In_ cl_image_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    if (!image)
        return CL_INVALID_MEM_OBJECT;

    return image->dispatch->clGetImageInfo(image,
                                           param_name,
                                           param_value_size,
                                           param_value,
                                           param_value_size_ret);
}

cl_int
CL_API_CALL
clGetImageInfo(
    _In_ cl_mem image,
    _In_ cl_image_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    return OclpTopDispatch->clGetImageInfo(image,
                                           param_name,
                                           param_value_size,
                                           param_value,
                                           param_value_size_ret);
}

static
cl_sampler
CL_API_CALL
OclpTermCreateSampler(
    _In_ cl_context context,
    _In_ cl_bool normalized_coords,
    _In_ cl_addressing_mode addressing_mode,
    _In_ cl_filter_mode filter_mode,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);

    return context->dispatch->clCreateSampler(context,
                                              normalized_coords,
                                              addressing_mode,
                                              filter_mode,
                                              errcode_ret);
}

cl_sampler
CL_API_CALL
clCreateSampler(
    _In_ cl_context context,
    _In_ cl_bool normalized_coords,
    _In_ cl_addressing_mode addressing_mode,
    _In_ cl_filter_mode filter_mode,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateSampler(context,
                                            normalized_coords,
                                            addressing_mode,
                                            filter_mode,
                                            errcode_ret);
}

static
cl_int
CL_API_CALL
OclpTermRetainSampler(
    _In_ cl_sampler sampler)
{
    if (!sampler)
        return CL_INVALID_SAMPLER;

    return sampler->dispatch->clRetainSampler(sampler);
}

cl_int
CL_API_CALL
clRetainSampler(
    _In_ cl_sampler sampler)
{
    return OclpTopDispatch->clRetainSampler(sampler);
}

static
cl_int
CL_API_CALL
OclpTermReleaseSampler(
    _In_ cl_sampler sampler)
{
    if (!sampler)
        return CL_INVALID_SAMPLER;

    return sampler->dispatch->clReleaseSampler(sampler);
}

cl_int
CL_API_CALL
clReleaseSampler(
    _In_ cl_sampler sampler)
{
    return OclpTopDispatch->clReleaseSampler(sampler);
}

static
cl_int
CL_API_CALL
OclpTermGetSamplerInfo(
    _In_ cl_sampler sampler,
    _In_ cl_sampler_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    if (!sampler)
        return CL_INVALID_SAMPLER;

    return sampler->dispatch->clGetSamplerInfo(sampler,
                                               param_name,
                                               param_value_size,
                                               param_value,
                                               param_value_size_ret);
}

cl_int
CL_API_CALL
clGetSamplerInfo(
    _In_ cl_sampler sampler,
    _In_ cl_sampler_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    return OclpTopDispatch->clGetSamplerInfo(sampler,
                                             param_name,
                                             param_value_size,
                                             param_value,
                                             param_value_size_ret);
}

static
cl_program
CL_API_CALL
OclpTermCreateProgramWithSource(
    _In_ cl_context context,
    _In_ cl_uint count,
    _In_reads_(count) const char** strings,
    _In_reads_opt_(count) const size_t* lengths,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);

    return context->dispatch->clCreateProgramWithSource(context,
                                                        count,
                                                        strings,
                                                        lengths,
                                                        errcode_ret);
}

cl_program
CL_API_CALL
clCreateProgramWithSource(
    _In_ cl_context context,
    _In_ cl_uint count,
    _In_reads_(count) const char** strings,
    _In_reads_opt_(count) const size_t* lengths,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateProgramWithSource(context,
                                                      count,
                                                      strings,
                                                      lengths,
                                                      errcode_ret);
}

static
cl_program
CL_API_CALL
OclpTermCreateProgramWithBinary(
    _In_ cl_context context,
    _In_ cl_uint num_devices,
    _In_reads_(num_devices) const cl_device_id* device_list,
    _In_reads_(num_devices) const size_t* lengths,
    _In_reads_(num_devices) const unsigned char** binaries,
    _Out_writes_opt_(num_devices) cl_int* binary_status,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);

    return context->dispatch->clCreateProgramWithBinary(context,
                                                        num_devices,
                                                        device_list,
                                                        lengths,
                                                        binaries,
                                                        binary_status,
                                                        errcode_ret);
}

cl_program
CL_API_CALL
clCreateProgramWithBinary(
    _In_ cl_context context,
    _In_ cl_uint num_devices,
    _In_reads_(num_devices) const cl_device_id* device_list,
    _In_reads_(num_devices) const size_t* lengths,
    _In_reads_(num_devices) const unsigned char** binaries,
    _Out_writes_opt_(num_devices) cl_int* binary_status,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateProgramWithBinary(context,
                                                      num_devices,
                                                      device_list,
                                                      lengths,
                                                      binaries,
                                                      binary_status,
                                                      errcode_ret);
}

static
cl_int
CL_API_CALL
OclpTermRetainProgram(
    _In_ cl_program program)
{
    if (!program)
        return CL_INVALID_PROGRAM;

    return program->dispatch->clRetainProgram(program);
}

cl_int
CL_API_CALL
clRetainProgram(
    _In_ cl_program program)
{
    return OclpTopDispatch->clRetainProgram(program);
}

static
cl_int
CL_API_CALL
OclpTermReleaseProgram(
    _In_ cl_program program)
{
    if (!program)
        return CL_INVALID_PROGRAM;

    return program->dispatch->clReleaseProgram(program);
}

cl_int
CL_API_CALL
clReleaseProgram(
    _In_ cl_program program)
{
    return OclpTopDispatch->clReleaseProgram(program);
}

static
cl_int
CL_API_CALL
OclpTermBuildProgram(
    _In_ cl_program program,
    _In_ cl_uint num_devices,
    _In_reads_opt_(num_devices) const cl_device_id* device_list,
    _In_opt_z_ const char* options,
    _In_opt_ void (CL_CALLBACK* pfn_notify)(cl_program program, void* user_data),
    _In_opt_ void* user_data)
{
    if (!program)
        return CL_INVALID_PROGRAM;

    return program->dispatch->clBuildProgram(program,
                                             num_devices,
                                             device_list,
                                             options,
                                             pfn_notify,
                                             user_data);
}

cl_int
CL_API_CALL
clBuildProgram(
    _In_ cl_program program,
    _In_ cl_uint num_devices,
    _In_reads_opt_(num_devices) const cl_device_id* device_list,
    _In_opt_z_ const char* options,
    _In_opt_ void (CL_CALLBACK* pfn_notify)(cl_program program, void* user_data),
    _In_opt_ void* user_data)
{
    return OclpTopDispatch->clBuildProgram(program,
                                           num_devices,
                                           device_list,
                                           options,
                                           pfn_notify,
                                           user_data);
}

static
cl_int
CL_API_CALL
OclpTermUnloadCompiler(void)
{
    return CL_SUCCESS;
}

cl_int
CL_API_CALL
clUnloadCompiler(void)
{
    return OclpTopDispatch->clUnloadCompiler();
}

static
cl_int
CL_API_CALL
OclpTermGetProgramInfo(
    _In_ cl_program program,
    _In_ cl_program_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    if (!program)
        return CL_INVALID_PROGRAM;

    return program->dispatch->clGetProgramInfo(program,
                                               param_name,
                                               param_value_size,
                                               param_value,
                                               param_value_size_ret);
}

cl_int
CL_API_CALL
clGetProgramInfo(
    _In_ cl_program program,
    _In_ cl_program_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    return OclpTopDispatch->clGetProgramInfo(program,
                                             param_name,
                                             param_value_size,
                                             param_value,
                                             param_value_size_ret);
}

static
cl_int
CL_API_CALL
OclpTermGetProgramBuildInfo(
    _In_ cl_program program,
    _In_ cl_device_id device,
    _In_ cl_program_build_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    if (!program)
        return CL_INVALID_PROGRAM;

    return program->dispatch->clGetProgramBuildInfo(program,
                                                    device,
                                                    param_name,
                                                    param_value_size,
                                                    param_value,
                                                    param_value_size_ret);
}

cl_int
CL_API_CALL
clGetProgramBuildInfo(
    _In_ cl_program program,
    _In_ cl_device_id device,
    _In_ cl_program_build_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    return OclpTopDispatch->clGetProgramBuildInfo(program,
                                                  device,
                                                  param_name,
                                                  param_value_size,
                                                  param_value,
                                                  param_value_size_ret);
}

static
cl_kernel
CL_API_CALL
OclpTermCreateKernel(
    _In_ cl_program program,
    _In_z_ const char* kernel_name,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!program)
        return OclpFailHandle(errcode_ret, CL_INVALID_PROGRAM);

    return program->dispatch->clCreateKernel(program, kernel_name, errcode_ret);
}

cl_kernel
CL_API_CALL
clCreateKernel(
    _In_ cl_program program,
    _In_z_ const char* kernel_name,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateKernel(program, kernel_name, errcode_ret);
}

static
cl_int
CL_API_CALL
OclpTermCreateKernelsInProgram(
    _In_ cl_program program,
    _In_ cl_uint num_kernels,
    _Out_writes_opt_(num_kernels) cl_kernel* kernels,
    _Out_opt_ cl_uint* num_kernels_ret)
{
    if (!program)
        return CL_INVALID_PROGRAM;

    return program->dispatch->clCreateKernelsInProgram(program,
                                                       num_kernels,
                                                       kernels,
                                                       num_kernels_ret);
}

cl_int
CL_API_CALL
clCreateKernelsInProgram(
    _In_ cl_program program,
    _In_ cl_uint num_kernels,
    _Out_writes_opt_(num_kernels) cl_kernel* kernels,
    _Out_opt_ cl_uint* num_kernels_ret)
{
    return OclpTopDispatch->clCreateKernelsInProgram(program,
                                                     num_kernels,
                                                     kernels,
                                                     num_kernels_ret);
}

static
cl_int
CL_API_CALL
OclpTermRetainKernel(
    _In_ cl_kernel kernel)
{
    if (!kernel)
        return CL_INVALID_KERNEL;

    return kernel->dispatch->clRetainKernel(kernel);
}

cl_int
CL_API_CALL
clRetainKernel(
    _In_ cl_kernel kernel)
{
    return OclpTopDispatch->clRetainKernel(kernel);
}

static
cl_int
CL_API_CALL
OclpTermReleaseKernel(
    _In_ cl_kernel kernel)
{
    if (!kernel)
        return CL_INVALID_KERNEL;

    return kernel->dispatch->clReleaseKernel(kernel);
}

cl_int
CL_API_CALL
clReleaseKernel(
    _In_ cl_kernel kernel)
{
    return OclpTopDispatch->clReleaseKernel(kernel);
}

static
cl_int
CL_API_CALL
OclpTermSetKernelArg(
    _In_ cl_kernel kernel,
    _In_ cl_uint arg_index,
    _In_ size_t arg_size,
    _In_reads_bytes_opt_(arg_size) const void* arg_value)
{
    if (!kernel)
        return CL_INVALID_KERNEL;

    return kernel->dispatch->clSetKernelArg(kernel, arg_index, arg_size, arg_value);
}

cl_int
CL_API_CALL
clSetKernelArg(
    _In_ cl_kernel kernel,
    _In_ cl_uint arg_index,
    _In_ size_t arg_size,
    _In_reads_bytes_opt_(arg_size) const void* arg_value)
{
    return OclpTopDispatch->clSetKernelArg(kernel, arg_index, arg_size, arg_value);
}

static
cl_int
CL_API_CALL
OclpTermGetKernelInfo(
    _In_ cl_kernel kernel,
    _In_ cl_kernel_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    if (!kernel)
        return CL_INVALID_KERNEL;

    return kernel->dispatch->clGetKernelInfo(kernel,
                                             param_name,
                                             param_value_size,
                                             param_value,
                                             param_value_size_ret);
}

cl_int
CL_API_CALL
clGetKernelInfo(
    _In_ cl_kernel kernel,
    _In_ cl_kernel_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    return OclpTopDispatch->clGetKernelInfo(kernel,
                                            param_name,
                                            param_value_size,
                                            param_value,
                                            param_value_size_ret);
}

static
cl_int
CL_API_CALL
OclpTermGetKernelWorkGroupInfo(
    _In_ cl_kernel kernel,
    _In_ cl_device_id device,
    _In_ cl_kernel_work_group_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    if (!kernel)
        return CL_INVALID_KERNEL;

    return kernel->dispatch->clGetKernelWorkGroupInfo(kernel,
                                                      device,
                                                      param_name,
                                                      param_value_size,
                                                      param_value,
                                                      param_value_size_ret);
}

cl_int
CL_API_CALL
clGetKernelWorkGroupInfo(
    _In_ cl_kernel kernel,
    _In_ cl_device_id device,
    _In_ cl_kernel_work_group_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    return OclpTopDispatch->clGetKernelWorkGroupInfo(kernel,
                                                     device,
                                                     param_name,
                                                     param_value_size,
                                                     param_value,
                                                     param_value_size_ret);
}

static
cl_int
CL_API_CALL
OclpTermWaitForEvents(
    _In_ cl_uint num_events,
    _In_reads_(num_events) const cl_event* event_list)
{
    if (!num_events || !event_list)
        return CL_INVALID_VALUE;
    if (!event_list[0])
        return CL_INVALID_EVENT;

    return event_list[0]->dispatch->clWaitForEvents(num_events, event_list);
}

cl_int
CL_API_CALL
clWaitForEvents(
    _In_ cl_uint num_events,
    _In_reads_(num_events) const cl_event* event_list)
{
    return OclpTopDispatch->clWaitForEvents(num_events, event_list);
}

static
cl_int
CL_API_CALL
OclpTermGetEventInfo(
    _In_ cl_event event,
    _In_ cl_event_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    if (!event)
        return CL_INVALID_EVENT;

    return event->dispatch->clGetEventInfo(event,
                                           param_name,
                                           param_value_size,
                                           param_value,
                                           param_value_size_ret);
}

cl_int
CL_API_CALL
clGetEventInfo(
    _In_ cl_event event,
    _In_ cl_event_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    return OclpTopDispatch->clGetEventInfo(event,
                                           param_name,
                                           param_value_size,
                                           param_value,
                                           param_value_size_ret);
}

static
cl_int
CL_API_CALL
OclpTermRetainEvent(
    _In_ cl_event event)
{
    if (!event)
        return CL_INVALID_EVENT;

    return event->dispatch->clRetainEvent(event);
}

cl_int
CL_API_CALL
clRetainEvent(
    _In_ cl_event event)
{
    return OclpTopDispatch->clRetainEvent(event);
}

static
cl_int
CL_API_CALL
OclpTermReleaseEvent(
    _In_ cl_event event)
{
    if (!event)
        return CL_INVALID_EVENT;

    return event->dispatch->clReleaseEvent(event);
}

cl_int
CL_API_CALL
clReleaseEvent(
    _In_ cl_event event)
{
    return OclpTopDispatch->clReleaseEvent(event);
}

static
cl_int
CL_API_CALL
OclpTermGetEventProfilingInfo(
    _In_ cl_event event,
    _In_ cl_profiling_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    if (!event)
        return CL_INVALID_EVENT;

    return event->dispatch->clGetEventProfilingInfo(event,
                                                    param_name,
                                                    param_value_size,
                                                    param_value,
                                                    param_value_size_ret);
}

cl_int
CL_API_CALL
clGetEventProfilingInfo(
    _In_ cl_event event,
    _In_ cl_profiling_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    return OclpTopDispatch->clGetEventProfilingInfo(event,
                                                    param_name,
                                                    param_value_size,
                                                    param_value,
                                                    param_value_size_ret);
}

static
cl_int
CL_API_CALL
OclpTermFlush(
    _In_ cl_command_queue command_queue)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clFlush(command_queue);
}

cl_int
CL_API_CALL
clFlush(
    _In_ cl_command_queue command_queue)
{
    return OclpTopDispatch->clFlush(command_queue);
}

static
cl_int
CL_API_CALL
OclpTermFinish(
    _In_ cl_command_queue command_queue)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clFinish(command_queue);
}

cl_int
CL_API_CALL
clFinish(
    _In_ cl_command_queue command_queue)
{
    return OclpTopDispatch->clFinish(command_queue);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueReadBuffer(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem buffer,
    _In_ cl_bool blocking_read,
    _In_ size_t offset,
    _In_ size_t size,
    _Out_writes_bytes_(size) void* ptr,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueReadBuffer(command_queue,
                                                        buffer,
                                                        blocking_read,
                                                        offset,
                                                        size,
                                                        ptr,
                                                        num_events_in_wait_list,
                                                        event_wait_list,
                                                        event);
}

cl_int
CL_API_CALL
clEnqueueReadBuffer(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem buffer,
    _In_ cl_bool blocking_read,
    _In_ size_t offset,
    _In_ size_t size,
    _Out_writes_bytes_(size) void* ptr,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueReadBuffer(command_queue,
                                                buffer,
                                                blocking_read,
                                                offset,
                                                size,
                                                ptr,
                                                num_events_in_wait_list,
                                                event_wait_list,
                                                event);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueWriteBuffer(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem buffer,
    _In_ cl_bool blocking_write,
    _In_ size_t offset,
    _In_ size_t size,
    _In_reads_bytes_(size) const void* ptr,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueWriteBuffer(command_queue,
                                                         buffer,
                                                         blocking_write,
                                                         offset,
                                                         size,
                                                         ptr,
                                                         num_events_in_wait_list,
                                                         event_wait_list,
                                                         event);
}

cl_int
CL_API_CALL
clEnqueueWriteBuffer(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem buffer,
    _In_ cl_bool blocking_write,
    _In_ size_t offset,
    _In_ size_t size,
    _In_reads_bytes_(size) const void* ptr,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueWriteBuffer(command_queue,
                                                 buffer,
                                                 blocking_write,
                                                 offset,
                                                 size,
                                                 ptr,
                                                 num_events_in_wait_list,
                                                 event_wait_list,
                                                 event);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueCopyBuffer(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem src_buffer,
    _In_ cl_mem dst_buffer,
    _In_ size_t src_offset,
    _In_ size_t dst_offset,
    _In_ size_t size,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueCopyBuffer(command_queue,
                                                        src_buffer,
                                                        dst_buffer,
                                                        src_offset,
                                                        dst_offset,
                                                        size,
                                                        num_events_in_wait_list,
                                                        event_wait_list,
                                                        event);
}

cl_int
CL_API_CALL
clEnqueueCopyBuffer(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem src_buffer,
    _In_ cl_mem dst_buffer,
    _In_ size_t src_offset,
    _In_ size_t dst_offset,
    _In_ size_t size,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueCopyBuffer(command_queue,
                                                src_buffer,
                                                dst_buffer,
                                                src_offset,
                                                dst_offset,
                                                size,
                                                num_events_in_wait_list,
                                                event_wait_list,
                                                event);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueReadImage(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem image,
    _In_ cl_bool blocking_read,
    _In_reads_(3) const size_t* origin,
    _In_reads_(3) const size_t* region,
    _In_ size_t row_pitch,
    _In_ size_t slice_pitch,
    _Out_ void* ptr,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueReadImage(command_queue,
                                                       image,
                                                       blocking_read,
                                                       origin,
                                                       region,
                                                       row_pitch,
                                                       slice_pitch,
                                                       ptr,
                                                       num_events_in_wait_list,
                                                       event_wait_list,
                                                       event);
}

cl_int
CL_API_CALL
clEnqueueReadImage(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem image,
    _In_ cl_bool blocking_read,
    _In_reads_(3) const size_t* origin,
    _In_reads_(3) const size_t* region,
    _In_ size_t row_pitch,
    _In_ size_t slice_pitch,
    _Out_ void* ptr,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueReadImage(command_queue,
                                               image,
                                               blocking_read,
                                               origin,
                                               region,
                                               row_pitch,
                                               slice_pitch,
                                               ptr,
                                               num_events_in_wait_list,
                                               event_wait_list,
                                               event);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueWriteImage(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem image,
    _In_ cl_bool blocking_write,
    _In_reads_(3) const size_t* origin,
    _In_reads_(3) const size_t* region,
    _In_ size_t input_row_pitch,
    _In_ size_t input_slice_pitch,
    _In_ const void* ptr,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueWriteImage(command_queue,
                                                        image,
                                                        blocking_write,
                                                        origin,
                                                        region,
                                                        input_row_pitch,
                                                        input_slice_pitch,
                                                        ptr,
                                                        num_events_in_wait_list,
                                                        event_wait_list,
                                                        event);
}

cl_int
CL_API_CALL
clEnqueueWriteImage(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem image,
    _In_ cl_bool blocking_write,
    _In_reads_(3) const size_t* origin,
    _In_reads_(3) const size_t* region,
    _In_ size_t input_row_pitch,
    _In_ size_t input_slice_pitch,
    _In_ const void* ptr,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueWriteImage(command_queue,
                                                image,
                                                blocking_write,
                                                origin,
                                                region,
                                                input_row_pitch,
                                                input_slice_pitch,
                                                ptr,
                                                num_events_in_wait_list,
                                                event_wait_list,
                                                event);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueCopyImage(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem src_image,
    _In_ cl_mem dst_image,
    _In_reads_(3) const size_t* src_origin,
    _In_reads_(3) const size_t* dst_origin,
    _In_reads_(3) const size_t* region,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueCopyImage(command_queue,
                                                       src_image,
                                                       dst_image,
                                                       src_origin,
                                                       dst_origin,
                                                       region,
                                                       num_events_in_wait_list,
                                                       event_wait_list,
                                                       event);
}

cl_int
CL_API_CALL
clEnqueueCopyImage(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem src_image,
    _In_ cl_mem dst_image,
    _In_reads_(3) const size_t* src_origin,
    _In_reads_(3) const size_t* dst_origin,
    _In_reads_(3) const size_t* region,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueCopyImage(command_queue,
                                               src_image,
                                               dst_image,
                                               src_origin,
                                               dst_origin,
                                               region,
                                               num_events_in_wait_list,
                                               event_wait_list,
                                               event);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueCopyImageToBuffer(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem src_image,
    _In_ cl_mem dst_buffer,
    _In_reads_(3) const size_t* src_origin,
    _In_reads_(3) const size_t* region,
    _In_ size_t dst_offset,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueCopyImageToBuffer(command_queue,
                                                               src_image,
                                                               dst_buffer,
                                                               src_origin,
                                                               region,
                                                               dst_offset,
                                                               num_events_in_wait_list,
                                                               event_wait_list,
                                                               event);
}

cl_int
CL_API_CALL
clEnqueueCopyImageToBuffer(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem src_image,
    _In_ cl_mem dst_buffer,
    _In_reads_(3) const size_t* src_origin,
    _In_reads_(3) const size_t* region,
    _In_ size_t dst_offset,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueCopyImageToBuffer(command_queue,
                                                       src_image,
                                                       dst_buffer,
                                                       src_origin,
                                                       region,
                                                       dst_offset,
                                                       num_events_in_wait_list,
                                                       event_wait_list,
                                                       event);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueCopyBufferToImage(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem src_buffer,
    _In_ cl_mem dst_image,
    _In_ size_t src_offset,
    _In_reads_(3) const size_t* dst_origin,
    _In_reads_(3) const size_t* region,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueCopyBufferToImage(command_queue,
                                                               src_buffer,
                                                               dst_image,
                                                               src_offset,
                                                               dst_origin,
                                                               region,
                                                               num_events_in_wait_list,
                                                               event_wait_list,
                                                               event);
}

cl_int
CL_API_CALL
clEnqueueCopyBufferToImage(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem src_buffer,
    _In_ cl_mem dst_image,
    _In_ size_t src_offset,
    _In_reads_(3) const size_t* dst_origin,
    _In_reads_(3) const size_t* region,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueCopyBufferToImage(command_queue,
                                                       src_buffer,
                                                       dst_image,
                                                       src_offset,
                                                       dst_origin,
                                                       region,
                                                       num_events_in_wait_list,
                                                       event_wait_list,
                                                       event);
}

static
void*
CL_API_CALL
OclpTermEnqueueMapBuffer(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem buffer,
    _In_ cl_bool blocking_map,
    _In_ cl_map_flags map_flags,
    _In_ size_t offset,
    _In_ size_t size,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!command_queue)
        return OclpFailHandle(errcode_ret, CL_INVALID_COMMAND_QUEUE);

    return command_queue->dispatch->clEnqueueMapBuffer(command_queue,
                                                       buffer,
                                                       blocking_map,
                                                       map_flags,
                                                       offset,
                                                       size,
                                                       num_events_in_wait_list,
                                                       event_wait_list,
                                                       event,
                                                       errcode_ret);
}

void*
CL_API_CALL
clEnqueueMapBuffer(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem buffer,
    _In_ cl_bool blocking_map,
    _In_ cl_map_flags map_flags,
    _In_ size_t offset,
    _In_ size_t size,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clEnqueueMapBuffer(command_queue,
                                               buffer,
                                               blocking_map,
                                               map_flags,
                                               offset,
                                               size,
                                               num_events_in_wait_list,
                                               event_wait_list,
                                               event,
                                               errcode_ret);
}

static
void*
CL_API_CALL
OclpTermEnqueueMapImage(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem image,
    _In_ cl_bool blocking_map,
    _In_ cl_map_flags map_flags,
    _In_reads_(3) const size_t* origin,
    _In_reads_(3) const size_t* region,
    _Out_ size_t* image_row_pitch,
    _Out_opt_ size_t* image_slice_pitch,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!command_queue)
        return OclpFailHandle(errcode_ret, CL_INVALID_COMMAND_QUEUE);

    return command_queue->dispatch->clEnqueueMapImage(command_queue,
                                                      image,
                                                      blocking_map,
                                                      map_flags,
                                                      origin,
                                                      region,
                                                      image_row_pitch,
                                                      image_slice_pitch,
                                                      num_events_in_wait_list,
                                                      event_wait_list,
                                                      event,
                                                      errcode_ret);
}

void*
CL_API_CALL
clEnqueueMapImage(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem image,
    _In_ cl_bool blocking_map,
    _In_ cl_map_flags map_flags,
    _In_reads_(3) const size_t* origin,
    _In_reads_(3) const size_t* region,
    _Out_ size_t* image_row_pitch,
    _Out_opt_ size_t* image_slice_pitch,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clEnqueueMapImage(command_queue,
                                              image,
                                              blocking_map,
                                              map_flags,
                                              origin,
                                              region,
                                              image_row_pitch,
                                              image_slice_pitch,
                                              num_events_in_wait_list,
                                              event_wait_list,
                                              event,
                                              errcode_ret);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueUnmapMemObject(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem memobj,
    _In_ void* mapped_ptr,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueUnmapMemObject(command_queue,
                                                            memobj,
                                                            mapped_ptr,
                                                            num_events_in_wait_list,
                                                            event_wait_list,
                                                            event);
}

cl_int
CL_API_CALL
clEnqueueUnmapMemObject(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem memobj,
    _In_ void* mapped_ptr,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueUnmapMemObject(command_queue,
                                                    memobj,
                                                    mapped_ptr,
                                                    num_events_in_wait_list,
                                                    event_wait_list,
                                                    event);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueNDRangeKernel(
    _In_ cl_command_queue command_queue,
    _In_ cl_kernel kernel,
    _In_ cl_uint work_dim,
    _In_reads_opt_(work_dim) const size_t* global_work_offset,
    _In_reads_(work_dim) const size_t* global_work_size,
    _In_reads_opt_(work_dim) const size_t* local_work_size,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueNDRangeKernel(command_queue,
                                                           kernel,
                                                           work_dim,
                                                           global_work_offset,
                                                           global_work_size,
                                                           local_work_size,
                                                           num_events_in_wait_list,
                                                           event_wait_list,
                                                           event);
}

cl_int
CL_API_CALL
clEnqueueNDRangeKernel(
    _In_ cl_command_queue command_queue,
    _In_ cl_kernel kernel,
    _In_ cl_uint work_dim,
    _In_reads_opt_(work_dim) const size_t* global_work_offset,
    _In_reads_(work_dim) const size_t* global_work_size,
    _In_reads_opt_(work_dim) const size_t* local_work_size,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueNDRangeKernel(command_queue,
                                                   kernel,
                                                   work_dim,
                                                   global_work_offset,
                                                   global_work_size,
                                                   local_work_size,
                                                   num_events_in_wait_list,
                                                   event_wait_list,
                                                   event);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueTask(
    _In_ cl_command_queue command_queue,
    _In_ cl_kernel kernel,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueTask(command_queue,
                                                  kernel,
                                                  num_events_in_wait_list,
                                                  event_wait_list,
                                                  event);
}

cl_int
CL_API_CALL
clEnqueueTask(
    _In_ cl_command_queue command_queue,
    _In_ cl_kernel kernel,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueTask(command_queue,
                                          kernel,
                                          num_events_in_wait_list,
                                          event_wait_list,
                                          event);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueNativeKernel(
    _In_ cl_command_queue command_queue,
    _In_ void (CL_CALLBACK* user_func)(void*),
    _In_reads_bytes_opt_(cb_args) void* args,
    _In_ size_t cb_args,
    _In_ cl_uint num_mem_objects,
    _In_reads_opt_(num_mem_objects) const cl_mem* mem_list,
    _In_reads_opt_(num_mem_objects) const void** args_mem_loc,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueNativeKernel(command_queue,
                                                          user_func,
                                                          args,
                                                          cb_args,
                                                          num_mem_objects,
                                                          mem_list,
                                                          args_mem_loc,
                                                          num_events_in_wait_list,
                                                          event_wait_list,
                                                          event);
}

cl_int
CL_API_CALL
clEnqueueNativeKernel(
    _In_ cl_command_queue command_queue,
    _In_ void (CL_CALLBACK* user_func)(void*),
    _In_reads_bytes_opt_(cb_args) void* args,
    _In_ size_t cb_args,
    _In_ cl_uint num_mem_objects,
    _In_reads_opt_(num_mem_objects) const cl_mem* mem_list,
    _In_reads_opt_(num_mem_objects) const void** args_mem_loc,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueNativeKernel(command_queue,
                                                  user_func,
                                                  args,
                                                  cb_args,
                                                  num_mem_objects,
                                                  mem_list,
                                                  args_mem_loc,
                                                  num_events_in_wait_list,
                                                  event_wait_list,
                                                  event);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueMarker(
    _In_ cl_command_queue command_queue,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueMarker(command_queue, event);
}

cl_int
CL_API_CALL
clEnqueueMarker(
    _In_ cl_command_queue command_queue,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueMarker(command_queue, event);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueWaitForEvents(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_events,
    _In_reads_(num_events) const cl_event* event_list)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueWaitForEvents(command_queue, num_events, event_list);
}

cl_int
CL_API_CALL
clEnqueueWaitForEvents(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_events,
    _In_reads_(num_events) const cl_event* event_list)
{
    return OclpTopDispatch->clEnqueueWaitForEvents(command_queue, num_events, event_list);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueBarrier(
    _In_ cl_command_queue command_queue)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueBarrier(command_queue);
}

cl_int
CL_API_CALL
clEnqueueBarrier(
    _In_ cl_command_queue command_queue)
{
    return OclpTopDispatch->clEnqueueBarrier(command_queue);
}

void*
CL_API_CALL
clGetExtensionFunctionAddress(
    _In_z_ const char* func_name)
{
    OclpInitialize();

    return OclpTopDispatch->clGetExtensionFunctionAddress(func_name);
}

static
cl_mem
CL_API_CALL
OclpTermCreateFromGLBuffer(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ cl_GLuint bufobj,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);
    if (!context->dispatch->clCreateFromGLBuffer)
        return OclpFailHandle(errcode_ret, CL_INVALID_OPERATION);

    return context->dispatch->clCreateFromGLBuffer(context, flags, bufobj, errcode_ret);
}

cl_mem
CL_API_CALL
clCreateFromGLBuffer(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ cl_GLuint bufobj,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateFromGLBuffer(context, flags, bufobj, errcode_ret);
}

static
cl_mem
CL_API_CALL
OclpTermCreateFromGLTexture2D(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ cl_GLenum target,
    _In_ cl_GLint miplevel,
    _In_ cl_GLuint texture,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);
    if (!context->dispatch->clCreateFromGLTexture2D)
        return OclpFailHandle(errcode_ret, CL_INVALID_OPERATION);

    return context->dispatch->clCreateFromGLTexture2D(context,
                                                      flags,
                                                      target,
                                                      miplevel,
                                                      texture,
                                                      errcode_ret);
}

cl_mem
CL_API_CALL
clCreateFromGLTexture2D(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ cl_GLenum target,
    _In_ cl_GLint miplevel,
    _In_ cl_GLuint texture,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateFromGLTexture2D(context,
                                                    flags,
                                                    target,
                                                    miplevel,
                                                    texture,
                                                    errcode_ret);
}

static
cl_mem
CL_API_CALL
OclpTermCreateFromGLTexture3D(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ cl_GLenum target,
    _In_ cl_GLint miplevel,
    _In_ cl_GLuint texture,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);
    if (!context->dispatch->clCreateFromGLTexture3D)
        return OclpFailHandle(errcode_ret, CL_INVALID_OPERATION);

    return context->dispatch->clCreateFromGLTexture3D(context,
                                                      flags,
                                                      target,
                                                      miplevel,
                                                      texture,
                                                      errcode_ret);
}

cl_mem
CL_API_CALL
clCreateFromGLTexture3D(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ cl_GLenum target,
    _In_ cl_GLint miplevel,
    _In_ cl_GLuint texture,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateFromGLTexture3D(context,
                                                    flags,
                                                    target,
                                                    miplevel,
                                                    texture,
                                                    errcode_ret);
}

static
cl_mem
CL_API_CALL
OclpTermCreateFromGLRenderbuffer(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ cl_GLuint renderbuffer,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);
    if (!context->dispatch->clCreateFromGLRenderbuffer)
        return OclpFailHandle(errcode_ret, CL_INVALID_OPERATION);

    return context->dispatch->clCreateFromGLRenderbuffer(context, flags, renderbuffer, errcode_ret);
}

cl_mem
CL_API_CALL
clCreateFromGLRenderbuffer(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ cl_GLuint renderbuffer,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateFromGLRenderbuffer(context, flags, renderbuffer, errcode_ret);
}

static
cl_int
CL_API_CALL
OclpTermGetGLObjectInfo(
    _In_ cl_mem memobj,
    _Out_opt_ cl_gl_object_type* gl_object_type,
    _Out_opt_ cl_GLuint* gl_object_name)
{
    if (!memobj)
        return CL_INVALID_MEM_OBJECT;
    if (!memobj->dispatch->clGetGLObjectInfo)
        return CL_INVALID_OPERATION;

    return memobj->dispatch->clGetGLObjectInfo(memobj, gl_object_type, gl_object_name);
}

cl_int
CL_API_CALL
clGetGLObjectInfo(
    _In_ cl_mem memobj,
    _Out_opt_ cl_gl_object_type* gl_object_type,
    _Out_opt_ cl_GLuint* gl_object_name)
{
    return OclpTopDispatch->clGetGLObjectInfo(memobj, gl_object_type, gl_object_name);
}

static
cl_int
CL_API_CALL
OclpTermGetGLTextureInfo(
    _In_ cl_mem memobj,
    _In_ cl_gl_texture_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    if (!memobj)
        return CL_INVALID_MEM_OBJECT;
    if (!memobj->dispatch->clGetGLTextureInfo)
        return CL_INVALID_OPERATION;

    return memobj->dispatch->clGetGLTextureInfo(memobj,
                                                param_name,
                                                param_value_size,
                                                param_value,
                                                param_value_size_ret);
}

cl_int
CL_API_CALL
clGetGLTextureInfo(
    _In_ cl_mem memobj,
    _In_ cl_gl_texture_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    return OclpTopDispatch->clGetGLTextureInfo(memobj,
                                               param_name,
                                               param_value_size,
                                               param_value,
                                               param_value_size_ret);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueAcquireGLObjects(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_objects,
    _In_reads_(num_objects) const cl_mem* mem_objects,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;
    if (!command_queue->dispatch->clEnqueueAcquireGLObjects)
        return CL_INVALID_OPERATION;

    return command_queue->dispatch->clEnqueueAcquireGLObjects(command_queue,
                                                              num_objects,
                                                              mem_objects,
                                                              num_events_in_wait_list,
                                                              event_wait_list,
                                                              event);
}

cl_int
CL_API_CALL
clEnqueueAcquireGLObjects(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_objects,
    _In_reads_(num_objects) const cl_mem* mem_objects,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueAcquireGLObjects(command_queue,
                                                      num_objects,
                                                      mem_objects,
                                                      num_events_in_wait_list,
                                                      event_wait_list,
                                                      event);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueReleaseGLObjects(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_objects,
    _In_reads_(num_objects) const cl_mem* mem_objects,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;
    if (!command_queue->dispatch->clEnqueueReleaseGLObjects)
        return CL_INVALID_OPERATION;

    return command_queue->dispatch->clEnqueueReleaseGLObjects(command_queue,
                                                              num_objects,
                                                              mem_objects,
                                                              num_events_in_wait_list,
                                                              event_wait_list,
                                                              event);
}

cl_int
CL_API_CALL
clEnqueueReleaseGLObjects(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_objects,
    _In_reads_(num_objects) const cl_mem* mem_objects,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueReleaseGLObjects(command_queue,
                                                      num_objects,
                                                      mem_objects,
                                                      num_events_in_wait_list,
                                                      event_wait_list,
                                                      event);
}

static
cl_int
CL_API_CALL
OclpTermSetEventCallback(
    _In_ cl_event event,
    _In_ cl_int command_exec_callback_type,
    _In_opt_ void (CL_CALLBACK* pfn_notify)(cl_event event, cl_int event_command_status, void *user_data),
    _In_opt_ void* user_data)
{
    if (!event)
        return CL_INVALID_EVENT;

    return event->dispatch->clSetEventCallback(event,
                                               command_exec_callback_type,
                                               pfn_notify,
                                               user_data);
}

cl_int
CL_API_CALL
clSetEventCallback(
    _In_ cl_event event,
    _In_ cl_int command_exec_callback_type,
    _In_opt_ void (CL_CALLBACK* pfn_notify)(cl_event event, cl_int event_command_status, void *user_data),
    _In_opt_ void* user_data)
{
    return OclpTopDispatch->clSetEventCallback(event,
                                               command_exec_callback_type,
                                               pfn_notify,
                                               user_data);
}

static
cl_mem
CL_API_CALL
OclpTermCreateSubBuffer(
    _In_ cl_mem buffer,
    _In_ cl_mem_flags flags,
    _In_ cl_buffer_create_type buffer_create_type,
    _In_ const void* buffer_create_info,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!buffer)
        return OclpFailHandle(errcode_ret, CL_INVALID_MEM_OBJECT);

    return buffer->dispatch->clCreateSubBuffer(buffer,
                                               flags,
                                               buffer_create_type,
                                               buffer_create_info,
                                               errcode_ret);
}

cl_mem
CL_API_CALL
clCreateSubBuffer(
    _In_ cl_mem buffer,
    _In_ cl_mem_flags flags,
    _In_ cl_buffer_create_type buffer_create_type,
    _In_ const void* buffer_create_info,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateSubBuffer(buffer,
                                              flags,
                                              buffer_create_type,
                                              buffer_create_info,
                                              errcode_ret);
}

static
cl_int
CL_API_CALL
OclpTermSetMemObjectDestructorCallback(
    _In_ cl_mem memobj,
    _In_opt_ void (CL_CALLBACK* pfn_notify)(cl_mem memobj, void* user_data),
    _In_opt_ void* user_data)
{
    if (!memobj)
        return CL_INVALID_MEM_OBJECT;

    return memobj->dispatch->clSetMemObjectDestructorCallback(memobj, pfn_notify, user_data);
}

cl_int
CL_API_CALL
clSetMemObjectDestructorCallback(
    _In_ cl_mem memobj,
    _In_opt_ void (CL_CALLBACK* pfn_notify)(cl_mem memobj, void* user_data),
    _In_opt_ void* user_data)
{
    return OclpTopDispatch->clSetMemObjectDestructorCallback(memobj, pfn_notify, user_data);
}

static
cl_event
CL_API_CALL
OclpTermCreateUserEvent(
    _In_ cl_context context,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);

    return context->dispatch->clCreateUserEvent(context, errcode_ret);
}

cl_event
CL_API_CALL
clCreateUserEvent(
    _In_ cl_context context,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateUserEvent(context, errcode_ret);
}

static
cl_int
CL_API_CALL
OclpTermSetUserEventStatus(
    _In_ cl_event event,
    _In_ cl_int execution_status)
{
    if (!event)
        return CL_INVALID_EVENT;

    return event->dispatch->clSetUserEventStatus(event, execution_status);
}

cl_int
CL_API_CALL
clSetUserEventStatus(
    _In_ cl_event event,
    _In_ cl_int execution_status)
{
    return OclpTopDispatch->clSetUserEventStatus(event, execution_status);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueReadBufferRect(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem buffer,
    _In_ cl_bool blocking_read,
    _In_reads_(3) const size_t* buffer_origin,
    _In_reads_(3) const size_t* host_origin,
    _In_reads_(3) const size_t* region,
    _In_ size_t buffer_row_pitch,
    _In_ size_t buffer_slice_pitch,
    _In_ size_t host_row_pitch,
    _In_ size_t host_slice_pitch,
    _Out_ void* ptr,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueReadBufferRect(command_queue,
                                                            buffer,
                                                            blocking_read,
                                                            buffer_origin,
                                                            host_origin,
                                                            region,
                                                            buffer_row_pitch,
                                                            buffer_slice_pitch,
                                                            host_row_pitch,
                                                            host_slice_pitch,
                                                            ptr,
                                                            num_events_in_wait_list,
                                                            event_wait_list,
                                                            event);
}

cl_int
CL_API_CALL
clEnqueueReadBufferRect(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem buffer,
    _In_ cl_bool blocking_read,
    _In_reads_(3) const size_t* buffer_origin,
    _In_reads_(3) const size_t* host_origin,
    _In_reads_(3) const size_t* region,
    _In_ size_t buffer_row_pitch,
    _In_ size_t buffer_slice_pitch,
    _In_ size_t host_row_pitch,
    _In_ size_t host_slice_pitch,
    _Out_ void* ptr,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueReadBufferRect(command_queue,
                                                    buffer,
                                                    blocking_read,
                                                    buffer_origin,
                                                    host_origin,
                                                    region,
                                                    buffer_row_pitch,
                                                    buffer_slice_pitch,
                                                    host_row_pitch,
                                                    host_slice_pitch,
                                                    ptr,
                                                    num_events_in_wait_list,
                                                    event_wait_list,
                                                    event);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueWriteBufferRect(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem buffer,
    _In_ cl_bool blocking_write,
    _In_reads_(3) const size_t* buffer_origin,
    _In_reads_(3) const size_t* host_origin,
    _In_reads_(3) const size_t* region,
    _In_ size_t buffer_row_pitch,
    _In_ size_t buffer_slice_pitch,
    _In_ size_t host_row_pitch,
    _In_ size_t host_slice_pitch,
    _In_ const void* ptr,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueWriteBufferRect(command_queue,
                                                             buffer,
                                                             blocking_write,
                                                             buffer_origin,
                                                             host_origin,
                                                             region,
                                                             buffer_row_pitch,
                                                             buffer_slice_pitch,
                                                             host_row_pitch,
                                                             host_slice_pitch,
                                                             ptr,
                                                             num_events_in_wait_list,
                                                             event_wait_list,
                                                             event);
}

cl_int
CL_API_CALL
clEnqueueWriteBufferRect(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem buffer,
    _In_ cl_bool blocking_write,
    _In_reads_(3) const size_t* buffer_origin,
    _In_reads_(3) const size_t* host_origin,
    _In_reads_(3) const size_t* region,
    _In_ size_t buffer_row_pitch,
    _In_ size_t buffer_slice_pitch,
    _In_ size_t host_row_pitch,
    _In_ size_t host_slice_pitch,
    _In_ const void* ptr,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueWriteBufferRect(command_queue,
                                                     buffer,
                                                     blocking_write,
                                                     buffer_origin,
                                                     host_origin,
                                                     region,
                                                     buffer_row_pitch,
                                                     buffer_slice_pitch,
                                                     host_row_pitch,
                                                     host_slice_pitch,
                                                     ptr,
                                                     num_events_in_wait_list,
                                                     event_wait_list,
                                                     event);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueCopyBufferRect(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem src_buffer,
    _In_ cl_mem dst_buffer,
    _In_reads_(3) const size_t* src_origin,
    _In_reads_(3) const size_t* dst_origin,
    _In_reads_(3) const size_t* region,
    _In_ size_t src_row_pitch,
    _In_ size_t src_slice_pitch,
    _In_ size_t dst_row_pitch,
    _In_ size_t dst_slice_pitch,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueCopyBufferRect(command_queue,
                                                            src_buffer,
                                                            dst_buffer,
                                                            src_origin,
                                                            dst_origin,
                                                            region,
                                                            src_row_pitch,
                                                            src_slice_pitch,
                                                            dst_row_pitch,
                                                            dst_slice_pitch,
                                                            num_events_in_wait_list,
                                                            event_wait_list,
                                                            event);
}

cl_int
CL_API_CALL
clEnqueueCopyBufferRect(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem src_buffer,
    _In_ cl_mem dst_buffer,
    _In_reads_(3) const size_t* src_origin,
    _In_reads_(3) const size_t* dst_origin,
    _In_reads_(3) const size_t* region,
    _In_ size_t src_row_pitch,
    _In_ size_t src_slice_pitch,
    _In_ size_t dst_row_pitch,
    _In_ size_t dst_slice_pitch,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueCopyBufferRect(command_queue,
                                                    src_buffer,
                                                    dst_buffer,
                                                    src_origin,
                                                    dst_origin,
                                                    region,
                                                    src_row_pitch,
                                                    src_slice_pitch,
                                                    dst_row_pitch,
                                                    dst_slice_pitch,
                                                    num_events_in_wait_list,
                                                    event_wait_list,
                                                    event);
}

static
cl_int
CL_API_CALL
OclpTermCreateSubDevices(
    _In_ cl_device_id in_device,
    _In_ const cl_device_partition_property* properties,
    _In_ cl_uint num_devices,
    _Out_writes_opt_(num_devices) cl_device_id* out_devices,
    _Out_opt_ cl_uint* num_devices_ret)
{
    if (!in_device)
        return CL_INVALID_DEVICE;

    return in_device->dispatch->clCreateSubDevices(in_device,
                                                   properties,
                                                   num_devices,
                                                   out_devices,
                                                   num_devices_ret);
}

cl_int
CL_API_CALL
clCreateSubDevices(
    _In_ cl_device_id in_device,
    _In_ const cl_device_partition_property* properties,
    _In_ cl_uint num_devices,
    _Out_writes_opt_(num_devices) cl_device_id* out_devices,
    _Out_opt_ cl_uint* num_devices_ret)
{
    return OclpTopDispatch->clCreateSubDevices(in_device,
                                               properties,
                                               num_devices,
                                               out_devices,
                                               num_devices_ret);
}

static
cl_int
CL_API_CALL
OclpTermRetainDevice(
    _In_ cl_device_id device)
{
    if (!device)
        return CL_INVALID_DEVICE;

    return device->dispatch->clRetainDevice(device);
}

cl_int
CL_API_CALL
clRetainDevice(
    _In_ cl_device_id device)
{
    return OclpTopDispatch->clRetainDevice(device);
}

static
cl_int
CL_API_CALL
OclpTermReleaseDevice(
    _In_ cl_device_id device)
{
    if (!device)
        return CL_INVALID_DEVICE;

    return device->dispatch->clReleaseDevice(device);
}

cl_int
CL_API_CALL
clReleaseDevice(
    _In_ cl_device_id device)
{
    return OclpTopDispatch->clReleaseDevice(device);
}

static
cl_mem
CL_API_CALL
OclpTermCreateImage(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ const cl_image_format* image_format,
    _In_ const cl_image_desc* image_desc,
    _In_opt_ void* host_ptr,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);

    return context->dispatch->clCreateImage(context,
                                            flags,
                                            image_format,
                                            image_desc,
                                            host_ptr,
                                            errcode_ret);
}

cl_mem
CL_API_CALL
clCreateImage(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ const cl_image_format* image_format,
    _In_ const cl_image_desc* image_desc,
    _In_opt_ void* host_ptr,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateImage(context,
                                          flags,
                                          image_format,
                                          image_desc,
                                          host_ptr,
                                          errcode_ret);
}

static
cl_program
CL_API_CALL
OclpTermCreateProgramWithBuiltInKernels(
    _In_ cl_context context,
    _In_ cl_uint num_devices,
    _In_reads_(num_devices) const cl_device_id* device_list,
    _In_z_ const char* kernel_names,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);

    return context->dispatch->clCreateProgramWithBuiltInKernels(context,
                                                                num_devices,
                                                                device_list,
                                                                kernel_names,
                                                                errcode_ret);
}

cl_program
CL_API_CALL
clCreateProgramWithBuiltInKernels(
    _In_ cl_context context,
    _In_ cl_uint num_devices,
    _In_reads_(num_devices) const cl_device_id* device_list,
    _In_z_ const char* kernel_names,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateProgramWithBuiltInKernels(context,
                                                              num_devices,
                                                              device_list,
                                                              kernel_names,
                                                              errcode_ret);
}

static
cl_int
CL_API_CALL
OclpTermCompileProgram(
    _In_ cl_program program,
    _In_ cl_uint num_devices,
    _In_reads_opt_(num_devices) const cl_device_id* device_list,
    _In_opt_z_ const char* options,
    _In_ cl_uint num_input_headers,
    _In_reads_opt_(num_input_headers) const cl_program* input_headers,
    _In_reads_opt_(num_input_headers) const char** header_include_names,
    _In_opt_ void (CL_CALLBACK* pfn_notify)(cl_program program, void* user_data),
    _In_opt_ void* user_data)
{
    if (!program)
        return CL_INVALID_PROGRAM;

    return program->dispatch->clCompileProgram(program,
                                               num_devices,
                                               device_list,
                                               options,
                                               num_input_headers,
                                               input_headers,
                                               header_include_names,
                                               pfn_notify,
                                               user_data);
}

cl_int
CL_API_CALL
clCompileProgram(
    _In_ cl_program program,
    _In_ cl_uint num_devices,
    _In_reads_opt_(num_devices) const cl_device_id* device_list,
    _In_opt_z_ const char* options,
    _In_ cl_uint num_input_headers,
    _In_reads_opt_(num_input_headers) const cl_program* input_headers,
    _In_reads_opt_(num_input_headers) const char** header_include_names,
    _In_opt_ void (CL_CALLBACK* pfn_notify)(cl_program program, void* user_data),
    _In_opt_ void* user_data)
{
    return OclpTopDispatch->clCompileProgram(program,
                                             num_devices,
                                             device_list,
                                             options,
                                             num_input_headers,
                                             input_headers,
                                             header_include_names,
                                             pfn_notify,
                                             user_data);
}

static
cl_program
CL_API_CALL
OclpTermLinkProgram(
    _In_ cl_context context,
    _In_ cl_uint num_devices,
    _In_reads_opt_(num_devices) const cl_device_id* device_list,
    _In_opt_z_ const char* options,
    _In_ cl_uint num_input_programs,
    _In_reads_(num_input_programs) const cl_program* input_programs,
    _In_opt_ void (CL_CALLBACK* pfn_notify)(cl_program program, void* user_data),
    _In_opt_ void* user_data,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);

    return context->dispatch->clLinkProgram(context,
                                            num_devices,
                                            device_list,
                                            options,
                                            num_input_programs,
                                            input_programs,
                                            pfn_notify,
                                            user_data,
                                            errcode_ret);
}

cl_program
CL_API_CALL
clLinkProgram(
    _In_ cl_context context,
    _In_ cl_uint num_devices,
    _In_reads_opt_(num_devices) const cl_device_id* device_list,
    _In_opt_z_ const char* options,
    _In_ cl_uint num_input_programs,
    _In_reads_(num_input_programs) const cl_program* input_programs,
    _In_opt_ void (CL_CALLBACK* pfn_notify)(cl_program program, void* user_data),
    _In_opt_ void* user_data,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clLinkProgram(context,
                                          num_devices,
                                          device_list,
                                          options,
                                          num_input_programs,
                                          input_programs,
                                          pfn_notify,
                                          user_data,
                                          errcode_ret);
}

static
cl_int
CL_API_CALL
OclpTermUnloadPlatformCompiler(
    _In_ cl_platform_id platform)
{
    if (!platform)
        return CL_INVALID_PLATFORM;

    return platform->dispatch->clUnloadPlatformCompiler(platform);
}

cl_int
CL_API_CALL
clUnloadPlatformCompiler(
    _In_ cl_platform_id platform)
{
    return OclpTopDispatch->clUnloadPlatformCompiler(platform);
}

static
cl_int
CL_API_CALL
OclpTermGetKernelArgInfo(
    _In_ cl_kernel kernel,
    _In_ cl_uint arg_index,
    _In_ cl_kernel_arg_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    if (!kernel)
        return CL_INVALID_KERNEL;

    return kernel->dispatch->clGetKernelArgInfo(kernel,
                                                arg_index,
                                                param_name,
                                                param_value_size,
                                                param_value,
                                                param_value_size_ret);
}

cl_int
CL_API_CALL
clGetKernelArgInfo(
    _In_ cl_kernel kernel,
    _In_ cl_uint arg_index,
    _In_ cl_kernel_arg_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    return OclpTopDispatch->clGetKernelArgInfo(kernel,
                                               arg_index,
                                               param_name,
                                               param_value_size,
                                               param_value,
                                               param_value_size_ret);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueFillBuffer(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem buffer,
    _In_reads_bytes_(pattern_size) const void* pattern,
    _In_ size_t pattern_size,
    _In_ size_t offset,
    _In_ size_t size,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueFillBuffer(command_queue,
                                                        buffer,
                                                        pattern,
                                                        pattern_size,
                                                        offset,
                                                        size,
                                                        num_events_in_wait_list,
                                                        event_wait_list,
                                                        event);
}

cl_int
CL_API_CALL
clEnqueueFillBuffer(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem buffer,
    _In_reads_bytes_(pattern_size) const void* pattern,
    _In_ size_t pattern_size,
    _In_ size_t offset,
    _In_ size_t size,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueFillBuffer(command_queue,
                                                buffer,
                                                pattern,
                                                pattern_size,
                                                offset,
                                                size,
                                                num_events_in_wait_list,
                                                event_wait_list,
                                                event);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueFillImage(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem image,
    _In_ const void* fill_color,
    _In_reads_(3) const size_t* origin,
    _In_reads_(3) const size_t* region,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueFillImage(command_queue,
                                                       image,
                                                       fill_color,
                                                       origin,
                                                       region,
                                                       num_events_in_wait_list,
                                                       event_wait_list,
                                                       event);
}

cl_int
CL_API_CALL
clEnqueueFillImage(
    _In_ cl_command_queue command_queue,
    _In_ cl_mem image,
    _In_ const void* fill_color,
    _In_reads_(3) const size_t* origin,
    _In_reads_(3) const size_t* region,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueFillImage(command_queue,
                                               image,
                                               fill_color,
                                               origin,
                                               region,
                                               num_events_in_wait_list,
                                               event_wait_list,
                                               event);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueMigrateMemObjects(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_mem_objects,
    _In_reads_(num_mem_objects) const cl_mem* mem_objects,
    _In_ cl_mem_migration_flags flags,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueMigrateMemObjects(command_queue,
                                                               num_mem_objects,
                                                               mem_objects,
                                                               flags,
                                                               num_events_in_wait_list,
                                                               event_wait_list,
                                                               event);
}

cl_int
CL_API_CALL
clEnqueueMigrateMemObjects(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_mem_objects,
    _In_reads_(num_mem_objects) const cl_mem* mem_objects,
    _In_ cl_mem_migration_flags flags,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueMigrateMemObjects(command_queue,
                                                       num_mem_objects,
                                                       mem_objects,
                                                       flags,
                                                       num_events_in_wait_list,
                                                       event_wait_list,
                                                       event);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueMarkerWithWaitList(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueMarkerWithWaitList(command_queue,
                                                                num_events_in_wait_list,
                                                                event_wait_list,
                                                                event);
}

cl_int
CL_API_CALL
clEnqueueMarkerWithWaitList(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueMarkerWithWaitList(command_queue,
                                                        num_events_in_wait_list,
                                                        event_wait_list,
                                                        event);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueBarrierWithWaitList(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueBarrierWithWaitList(command_queue,
                                                                 num_events_in_wait_list,
                                                                 event_wait_list,
                                                                 event);
}

cl_int
CL_API_CALL
clEnqueueBarrierWithWaitList(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueBarrierWithWaitList(command_queue,
                                                         num_events_in_wait_list,
                                                         event_wait_list,
                                                         event);
}

void*
CL_API_CALL
clGetExtensionFunctionAddressForPlatform(
    _In_ cl_platform_id platform,
    _In_z_ const char* func_name)
{
    OclpInitialize();

    return OclpTopDispatch->clGetExtensionFunctionAddressForPlatform(platform, func_name);
}

static
cl_mem
CL_API_CALL
OclpTermCreateFromGLTexture(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ cl_GLenum target,
    _In_ cl_GLint miplevel,
    _In_ cl_GLuint texture,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);
    if (!context->dispatch->clCreateFromGLTexture)
        return OclpFailHandle(errcode_ret, CL_INVALID_OPERATION);

    return context->dispatch->clCreateFromGLTexture(context,
                                                    flags,
                                                    target,
                                                    miplevel,
                                                    texture,
                                                    errcode_ret);
}

cl_mem
CL_API_CALL
clCreateFromGLTexture(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ cl_GLenum target,
    _In_ cl_GLint miplevel,
    _In_ cl_GLuint texture,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateFromGLTexture(context,
                                                  flags,
                                                  target,
                                                  miplevel,
                                                  texture,
                                                  errcode_ret);
}

static
cl_command_queue
CL_API_CALL
OclpTermCreateCommandQueueWithProperties(
    _In_ cl_context context,
    _In_ cl_device_id device,
    _In_opt_ const cl_queue_properties* properties,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);

    return context->dispatch->clCreateCommandQueueWithProperties(context,
                                                                 device,
                                                                 properties,
                                                                 errcode_ret);
}

cl_command_queue
CL_API_CALL
clCreateCommandQueueWithProperties(
    _In_ cl_context context,
    _In_ cl_device_id device,
    _In_opt_ const cl_queue_properties* properties,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateCommandQueueWithProperties(context,
                                                               device,
                                                               properties,
                                                               errcode_ret);
}

static
cl_mem
CL_API_CALL
OclpTermCreatePipe(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ cl_uint pipe_packet_size,
    _In_ cl_uint pipe_max_packets,
    _In_opt_ const cl_pipe_properties* properties,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);

    return context->dispatch->clCreatePipe(context,
                                           flags,
                                           pipe_packet_size,
                                           pipe_max_packets,
                                           properties,
                                           errcode_ret);
}

cl_mem
CL_API_CALL
clCreatePipe(
    _In_ cl_context context,
    _In_ cl_mem_flags flags,
    _In_ cl_uint pipe_packet_size,
    _In_ cl_uint pipe_max_packets,
    _In_opt_ const cl_pipe_properties* properties,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreatePipe(context,
                                         flags,
                                         pipe_packet_size,
                                         pipe_max_packets,
                                         properties,
                                         errcode_ret);
}

static
cl_int
CL_API_CALL
OclpTermGetPipeInfo(
    _In_ cl_mem pipe,
    _In_ cl_pipe_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    if (!pipe)
        return CL_INVALID_MEM_OBJECT;

    return pipe->dispatch->clGetPipeInfo(pipe,
                                         param_name,
                                         param_value_size,
                                         param_value,
                                         param_value_size_ret);
}

cl_int
CL_API_CALL
clGetPipeInfo(
    _In_ cl_mem pipe,
    _In_ cl_pipe_info param_name,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    return OclpTopDispatch->clGetPipeInfo(pipe,
                                          param_name,
                                          param_value_size,
                                          param_value,
                                          param_value_size_ret);
}

static
void*
CL_API_CALL
OclpTermSVMAlloc(
    _In_ cl_context context,
    _In_ cl_svm_mem_flags flags,
    _In_ size_t size,
    _In_ cl_uint alignment)
{
    if (!context)
        return NULL;

    return context->dispatch->clSVMAlloc(context, flags, size, alignment);
}

void*
CL_API_CALL
clSVMAlloc(
    _In_ cl_context context,
    _In_ cl_svm_mem_flags flags,
    _In_ size_t size,
    _In_ cl_uint alignment)
{
    return OclpTopDispatch->clSVMAlloc(context, flags, size, alignment);
}

static
void
CL_API_CALL
OclpTermSVMFree(
    _In_opt_ cl_context context,
    _In_opt_ void* svm_pointer)
{
    if (context)
        context->dispatch->clSVMFree(context, svm_pointer);
}

void
CL_API_CALL
clSVMFree(
    _In_opt_ cl_context context,
    _In_opt_ void* svm_pointer)
{
    OclpTopDispatch->clSVMFree(context, svm_pointer);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueSVMFree(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_svm_pointers,
    _In_reads_(num_svm_pointers) void* svm_pointers[],
    _In_opt_ void (CL_CALLBACK* pfn_free_func)(cl_command_queue queue, cl_uint num_svm_pointers, void* svm_pointers[], void* user_data),
    _In_opt_ void* user_data,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueSVMFree(command_queue,
                                                     num_svm_pointers,
                                                     svm_pointers,
                                                     pfn_free_func,
                                                     user_data,
                                                     num_events_in_wait_list,
                                                     event_wait_list,
                                                     event);
}

cl_int
CL_API_CALL
clEnqueueSVMFree(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_svm_pointers,
    _In_reads_(num_svm_pointers) void* svm_pointers[],
    _In_opt_ void (CL_CALLBACK* pfn_free_func)(cl_command_queue queue, cl_uint num_svm_pointers, void* svm_pointers[], void* user_data),
    _In_opt_ void* user_data,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueSVMFree(command_queue,
                                             num_svm_pointers,
                                             svm_pointers,
                                             pfn_free_func,
                                             user_data,
                                             num_events_in_wait_list,
                                             event_wait_list,
                                             event);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueSVMMemcpy(
    _In_ cl_command_queue command_queue,
    _In_ cl_bool blocking_copy,
    _Out_writes_bytes_(size) void* dst_ptr,
    _In_reads_bytes_(size) const void* src_ptr,
    _In_ size_t size,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueSVMMemcpy(command_queue,
                                                       blocking_copy,
                                                       dst_ptr,
                                                       src_ptr,
                                                       size,
                                                       num_events_in_wait_list,
                                                       event_wait_list,
                                                       event);
}

cl_int
CL_API_CALL
clEnqueueSVMMemcpy(
    _In_ cl_command_queue command_queue,
    _In_ cl_bool blocking_copy,
    _Out_writes_bytes_(size) void* dst_ptr,
    _In_reads_bytes_(size) const void* src_ptr,
    _In_ size_t size,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueSVMMemcpy(command_queue,
                                               blocking_copy,
                                               dst_ptr,
                                               src_ptr,
                                               size,
                                               num_events_in_wait_list,
                                               event_wait_list,
                                               event);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueSVMMemFill(
    _In_ cl_command_queue command_queue,
    _Out_writes_bytes_(size) void* svm_ptr,
    _In_reads_bytes_(pattern_size) const void* pattern,
    _In_ size_t pattern_size,
    _In_ size_t size,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueSVMMemFill(command_queue,
                                                        svm_ptr,
                                                        pattern,
                                                        pattern_size,
                                                        size,
                                                        num_events_in_wait_list,
                                                        event_wait_list,
                                                        event);
}

cl_int
CL_API_CALL
clEnqueueSVMMemFill(
    _In_ cl_command_queue command_queue,
    _Out_writes_bytes_(size) void* svm_ptr,
    _In_reads_bytes_(pattern_size) const void* pattern,
    _In_ size_t pattern_size,
    _In_ size_t size,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueSVMMemFill(command_queue,
                                                svm_ptr,
                                                pattern,
                                                pattern_size,
                                                size,
                                                num_events_in_wait_list,
                                                event_wait_list,
                                                event);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueSVMMap(
    _In_ cl_command_queue command_queue,
    _In_ cl_bool blocking_map,
    _In_ cl_map_flags flags,
    _In_ void* svm_ptr,
    _In_ size_t size,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueSVMMap(command_queue,
                                                    blocking_map,
                                                    flags,
                                                    svm_ptr,
                                                    size,
                                                    num_events_in_wait_list,
                                                    event_wait_list,
                                                    event);
}

cl_int
CL_API_CALL
clEnqueueSVMMap(
    _In_ cl_command_queue command_queue,
    _In_ cl_bool blocking_map,
    _In_ cl_map_flags flags,
    _In_ void* svm_ptr,
    _In_ size_t size,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueSVMMap(command_queue,
                                            blocking_map,
                                            flags,
                                            svm_ptr,
                                            size,
                                            num_events_in_wait_list,
                                            event_wait_list,
                                            event);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueSVMUnmap(
    _In_ cl_command_queue command_queue,
    _In_ void* svm_ptr,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueSVMUnmap(command_queue,
                                                      svm_ptr,
                                                      num_events_in_wait_list,
                                                      event_wait_list,
                                                      event);
}

cl_int
CL_API_CALL
clEnqueueSVMUnmap(
    _In_ cl_command_queue command_queue,
    _In_ void* svm_ptr,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueSVMUnmap(command_queue,
                                              svm_ptr,
                                              num_events_in_wait_list,
                                              event_wait_list,
                                              event);
}

static
cl_sampler
CL_API_CALL
OclpTermCreateSamplerWithProperties(
    _In_ cl_context context,
    _In_opt_ const cl_sampler_properties* sampler_properties,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);

    return context->dispatch->clCreateSamplerWithProperties(context,
                                                            sampler_properties,
                                                            errcode_ret);
}

cl_sampler
CL_API_CALL
clCreateSamplerWithProperties(
    _In_ cl_context context,
    _In_opt_ const cl_sampler_properties* sampler_properties,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateSamplerWithProperties(context, sampler_properties, errcode_ret);
}

static
cl_int
CL_API_CALL
OclpTermSetKernelArgSVMPointer(
    _In_ cl_kernel kernel,
    _In_ cl_uint arg_index,
    _In_opt_ const void* arg_value)
{
    if (!kernel)
        return CL_INVALID_KERNEL;

    return kernel->dispatch->clSetKernelArgSVMPointer(kernel, arg_index, arg_value);
}

cl_int
CL_API_CALL
clSetKernelArgSVMPointer(
    _In_ cl_kernel kernel,
    _In_ cl_uint arg_index,
    _In_opt_ const void* arg_value)
{
    return OclpTopDispatch->clSetKernelArgSVMPointer(kernel, arg_index, arg_value);
}

static
cl_int
CL_API_CALL
OclpTermSetKernelExecInfo(
    _In_ cl_kernel kernel,
    _In_ cl_kernel_exec_info param_name,
    _In_ size_t param_value_size,
    _In_reads_bytes_(param_value_size) const void* param_value)
{
    if (!kernel)
        return CL_INVALID_KERNEL;

    return kernel->dispatch->clSetKernelExecInfo(kernel, param_name, param_value_size, param_value);
}

cl_int
CL_API_CALL
clSetKernelExecInfo(
    _In_ cl_kernel kernel,
    _In_ cl_kernel_exec_info param_name,
    _In_ size_t param_value_size,
    _In_reads_bytes_(param_value_size) const void* param_value)
{
    return OclpTopDispatch->clSetKernelExecInfo(kernel, param_name, param_value_size, param_value);
}

static
cl_kernel
CL_API_CALL
OclpTermCloneKernel(
    _In_ cl_kernel source_kernel,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!source_kernel)
        return OclpFailHandle(errcode_ret, CL_INVALID_KERNEL);

    return source_kernel->dispatch->clCloneKernel(source_kernel, errcode_ret);
}

cl_kernel
CL_API_CALL
clCloneKernel(
    _In_ cl_kernel source_kernel,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCloneKernel(source_kernel, errcode_ret);
}

static
cl_program
CL_API_CALL
OclpTermCreateProgramWithIL(
    _In_ cl_context context,
    _In_reads_bytes_(length) const void* il,
    _In_ size_t length,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);

    return context->dispatch->clCreateProgramWithIL(context, il, length, errcode_ret);
}

cl_program
CL_API_CALL
clCreateProgramWithIL(
    _In_ cl_context context,
    _In_reads_bytes_(length) const void* il,
    _In_ size_t length,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateProgramWithIL(context, il, length, errcode_ret);
}

static
cl_int
CL_API_CALL
OclpTermEnqueueSVMMigrateMem(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_svm_pointers,
    _In_reads_(num_svm_pointers) const void** svm_pointers,
    _In_reads_opt_(num_svm_pointers) const size_t* sizes,
    _In_ cl_mem_migration_flags flags,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    if (!command_queue)
        return CL_INVALID_COMMAND_QUEUE;

    return command_queue->dispatch->clEnqueueSVMMigrateMem(command_queue,
                                                           num_svm_pointers,
                                                           svm_pointers,
                                                           sizes,
                                                           flags,
                                                           num_events_in_wait_list,
                                                           event_wait_list,
                                                           event);
}

cl_int
CL_API_CALL
clEnqueueSVMMigrateMem(
    _In_ cl_command_queue command_queue,
    _In_ cl_uint num_svm_pointers,
    _In_reads_(num_svm_pointers) const void** svm_pointers,
    _In_reads_opt_(num_svm_pointers) const size_t* sizes,
    _In_ cl_mem_migration_flags flags,
    _In_ cl_uint num_events_in_wait_list,
    _In_reads_opt_(num_events_in_wait_list) const cl_event* event_wait_list,
    _Out_opt_ cl_event* event)
{
    return OclpTopDispatch->clEnqueueSVMMigrateMem(command_queue,
                                                   num_svm_pointers,
                                                   svm_pointers,
                                                   sizes,
                                                   flags,
                                                   num_events_in_wait_list,
                                                   event_wait_list,
                                                   event);
}

static
cl_int
CL_API_CALL
OclpTermGetDeviceAndHostTimer(
    _In_ cl_device_id device,
    _Out_ cl_ulong* device_timestamp,
    _Out_ cl_ulong* host_timestamp)
{
    if (!device)
        return CL_INVALID_DEVICE;

    return device->dispatch->clGetDeviceAndHostTimer(device, device_timestamp, host_timestamp);
}

cl_int
CL_API_CALL
clGetDeviceAndHostTimer(
    _In_ cl_device_id device,
    _Out_ cl_ulong* device_timestamp,
    _Out_ cl_ulong* host_timestamp)
{
    return OclpTopDispatch->clGetDeviceAndHostTimer(device, device_timestamp, host_timestamp);
}

static
cl_int
CL_API_CALL
OclpTermGetHostTimer(
    _In_ cl_device_id device,
    _Out_ cl_ulong* host_timestamp)
{
    if (!device)
        return CL_INVALID_DEVICE;

    return device->dispatch->clGetHostTimer(device, host_timestamp);
}

cl_int
CL_API_CALL
clGetHostTimer(
    _In_ cl_device_id device,
    _Out_ cl_ulong* host_timestamp)
{
    return OclpTopDispatch->clGetHostTimer(device, host_timestamp);
}

static
cl_int
CL_API_CALL
OclpTermGetKernelSubGroupInfo(
    _In_ cl_kernel kernel,
    _In_ cl_device_id device,
    _In_ cl_kernel_sub_group_info param_name,
    _In_ size_t input_value_size,
    _In_reads_bytes_opt_(input_value_size) const void* input_value,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    if (!kernel)
        return CL_INVALID_KERNEL;

    return kernel->dispatch->clGetKernelSubGroupInfo(kernel,
                                                     device,
                                                     param_name,
                                                     input_value_size,
                                                     input_value,
                                                     param_value_size,
                                                     param_value,
                                                     param_value_size_ret);
}

cl_int
CL_API_CALL
clGetKernelSubGroupInfo(
    _In_ cl_kernel kernel,
    _In_ cl_device_id device,
    _In_ cl_kernel_sub_group_info param_name,
    _In_ size_t input_value_size,
    _In_reads_bytes_opt_(input_value_size) const void* input_value,
    _In_ size_t param_value_size,
    _Out_writes_bytes_opt_(param_value_size) void* param_value,
    _Out_opt_ size_t* param_value_size_ret)
{
    return OclpTopDispatch->clGetKernelSubGroupInfo(kernel,
                                                    device,
                                                    param_name,
                                                    input_value_size,
                                                    input_value,
                                                    param_value_size,
                                                    param_value,
                                                    param_value_size_ret);
}

static
cl_int
CL_API_CALL
OclpTermSetDefaultDeviceCommandQueue(
    _In_ cl_context context,
    _In_ cl_device_id device,
    _In_ cl_command_queue command_queue)
{
    if (!context)
        return CL_INVALID_CONTEXT;

    return context->dispatch->clSetDefaultDeviceCommandQueue(context, device, command_queue);
}

cl_int
CL_API_CALL
clSetDefaultDeviceCommandQueue(
    _In_ cl_context context,
    _In_ cl_device_id device,
    _In_ cl_command_queue command_queue)
{
    return OclpTopDispatch->clSetDefaultDeviceCommandQueue(context, device, command_queue);
}

static
cl_int
CL_API_CALL
OclpTermSetProgramReleaseCallback(
    _In_ cl_program program,
    _In_opt_ void (CL_CALLBACK* pfn_notify)(cl_program program, void* user_data),
    _In_opt_ void* user_data)
{
    if (!program)
        return CL_INVALID_PROGRAM;

    return program->dispatch->clSetProgramReleaseCallback(program, pfn_notify, user_data);
}

cl_int
CL_API_CALL
clSetProgramReleaseCallback(
    _In_ cl_program program,
    _In_opt_ void (CL_CALLBACK* pfn_notify)(cl_program program, void* user_data),
    _In_opt_ void* user_data)
{
    return OclpTopDispatch->clSetProgramReleaseCallback(program, pfn_notify, user_data);
}

static
cl_int
CL_API_CALL
OclpTermSetProgramSpecializationConstant(
    _In_ cl_program program,
    _In_ cl_uint spec_id,
    _In_ size_t spec_size,
    _In_reads_bytes_(spec_size) const void* spec_value)
{
    if (!program)
        return CL_INVALID_PROGRAM;

    return program->dispatch->clSetProgramSpecializationConstant(program,
                                                                 spec_id,
                                                                 spec_size,
                                                                 spec_value);
}

cl_int
CL_API_CALL
clSetProgramSpecializationConstant(
    _In_ cl_program program,
    _In_ cl_uint spec_id,
    _In_ size_t spec_size,
    _In_reads_bytes_(spec_size) const void* spec_value)
{
    return OclpTopDispatch->clSetProgramSpecializationConstant(program,
                                                               spec_id,
                                                               spec_size,
                                                               spec_value);
}

static
cl_mem
CL_API_CALL
OclpTermCreateBufferWithProperties(
    _In_ cl_context context,
    _In_opt_ const cl_mem_properties* properties,
    _In_ cl_mem_flags flags,
    _In_ size_t size,
    _In_opt_ void* host_ptr,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);

    return context->dispatch->clCreateBufferWithProperties(context,
                                                           properties,
                                                           flags,
                                                           size,
                                                           host_ptr,
                                                           errcode_ret);
}

cl_mem
CL_API_CALL
clCreateBufferWithProperties(
    _In_ cl_context context,
    _In_opt_ const cl_mem_properties* properties,
    _In_ cl_mem_flags flags,
    _In_ size_t size,
    _In_opt_ void* host_ptr,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateBufferWithProperties(context,
                                                         properties,
                                                         flags,
                                                         size,
                                                         host_ptr,
                                                         errcode_ret);
}

static
cl_mem
CL_API_CALL
OclpTermCreateImageWithProperties(
    _In_ cl_context context,
    _In_opt_ const cl_mem_properties* properties,
    _In_ cl_mem_flags flags,
    _In_ const cl_image_format* image_format,
    _In_ const cl_image_desc* image_desc,
    _In_opt_ void* host_ptr,
    _Out_opt_ cl_int* errcode_ret)
{
    if (!context)
        return OclpFailHandle(errcode_ret, CL_INVALID_CONTEXT);

    return context->dispatch->clCreateImageWithProperties(context,
                                                          properties,
                                                          flags,
                                                          image_format,
                                                          image_desc,
                                                          host_ptr,
                                                          errcode_ret);
}

cl_mem
CL_API_CALL
clCreateImageWithProperties(
    _In_ cl_context context,
    _In_opt_ const cl_mem_properties* properties,
    _In_ cl_mem_flags flags,
    _In_ const cl_image_format* image_format,
    _In_ const cl_image_desc* image_desc,
    _In_opt_ void* host_ptr,
    _Out_opt_ cl_int* errcode_ret)
{
    return OclpTopDispatch->clCreateImageWithProperties(context,
                                                        properties,
                                                        flags,
                                                        image_format,
                                                        image_desc,
                                                        host_ptr,
                                                        errcode_ret);
}

static
cl_int
CL_API_CALL
OclpTermSetContextDestructorCallback(
    _In_ cl_context context,
    _In_opt_ void (CL_CALLBACK* pfn_notify)(cl_context context, void* user_data),
    _In_opt_ void* user_data)
{
    if (!context)
        return CL_INVALID_CONTEXT;

    return context->dispatch->clSetContextDestructorCallback(context, pfn_notify, user_data);
}

cl_int
CL_API_CALL
clSetContextDestructorCallback(
    _In_ cl_context context,
    _In_opt_ void (CL_CALLBACK* pfn_notify)(cl_context context, void* user_data),
    _In_opt_ void* user_data)
{
    return OclpTopDispatch->clSetContextDestructorCallback(context, pfn_notify, user_data);
}

/* Loader end of the dispatch chain, handed to layers as their target */
const cl_icd_dispatch OclpLoaderDispatch =
{
    .clGetPlatformIDs = OclpTermGetPlatformIDs,
    .clGetPlatformInfo = OclpTermGetPlatformInfo,
    .clGetDeviceIDs = OclpTermGetDeviceIDs,
    .clGetDeviceInfo = OclpTermGetDeviceInfo,
    .clCreateContext = OclpTermCreateContext,
    .clCreateContextFromType = OclpTermCreateContextFromType,
    .clRetainContext = OclpTermRetainContext,
    .clReleaseContext = OclpTermReleaseContext,
    .clGetContextInfo = OclpTermGetContextInfo,
    .clCreateCommandQueue = OclpTermCreateCommandQueue,
    .clRetainCommandQueue = OclpTermRetainCommandQueue,
    .clReleaseCommandQueue = OclpTermReleaseCommandQueue,
    .clGetCommandQueueInfo = OclpTermGetCommandQueueInfo,
    .clSetCommandQueueProperty = OclpTermSetCommandQueueProperty,
    .clCreateBuffer = OclpTermCreateBuffer,
    .clCreateImage2D = OclpTermCreateImage2D,
    .clCreateImage3D = OclpTermCreateImage3D,
    .clRetainMemObject = OclpTermRetainMemObject,
    .clReleaseMemObject = OclpTermReleaseMemObject,
    .clGetSupportedImageFormats = OclpTermGetSupportedImageFormats,
    .clGetMemObjectInfo = OclpTermGetMemObjectInfo,
    .clGetImageInfo = OclpTermGetImageInfo,
    .clCreateSampler = OclpTermCreateSampler,
    .clRetainSampler = OclpTermRetainSampler,
    .clReleaseSampler = OclpTermReleaseSampler,
    .clGetSamplerInfo = OclpTermGetSamplerInfo,
    .clCreateProgramWithSource = OclpTermCreateProgramWithSource,
    .clCreateProgramWithBinary = OclpTermCreateProgramWithBinary,
    .clRetainProgram = OclpTermRetainProgram,
    .clReleaseProgram = OclpTermReleaseProgram,
    .clBuildProgram = OclpTermBuildProgram,
    .clUnloadCompiler = OclpTermUnloadCompiler,
    .clGetProgramInfo = OclpTermGetProgramInfo,
    .clGetProgramBuildInfo = OclpTermGetProgramBuildInfo,
    .clCreateKernel = OclpTermCreateKernel,
    .clCreateKernelsInProgram = OclpTermCreateKernelsInProgram,
    .clRetainKernel = OclpTermRetainKernel,
    .clReleaseKernel = OclpTermReleaseKernel,
    .clSetKernelArg = OclpTermSetKernelArg,
    .clGetKernelInfo = OclpTermGetKernelInfo,
    .clGetKernelWorkGroupInfo = OclpTermGetKernelWorkGroupInfo,
    .clWaitForEvents = OclpTermWaitForEvents,
    .clGetEventInfo = OclpTermGetEventInfo,
    .clRetainEvent = OclpTermRetainEvent,
    .clReleaseEvent = OclpTermReleaseEvent,
    .clGetEventProfilingInfo = OclpTermGetEventProfilingInfo,
    .clFlush = OclpTermFlush,
    .clFinish = OclpTermFinish,
    .clEnqueueReadBuffer = OclpTermEnqueueReadBuffer,
    .clEnqueueWriteBuffer = OclpTermEnqueueWriteBuffer,
    .clEnqueueCopyBuffer = OclpTermEnqueueCopyBuffer,
    .clEnqueueReadImage = OclpTermEnqueueReadImage,
    .clEnqueueWriteImage = OclpTermEnqueueWriteImage,
    .clEnqueueCopyImage = OclpTermEnqueueCopyImage,
    .clEnqueueCopyImageToBuffer = OclpTermEnqueueCopyImageToBuffer,
    .clEnqueueCopyBufferToImage = OclpTermEnqueueCopyBufferToImage,
    .clEnqueueMapBuffer = OclpTermEnqueueMapBuffer,
    .clEnqueueMapImage = OclpTermEnqueueMapImage,
    .clEnqueueUnmapMemObject = OclpTermEnqueueUnmapMemObject,
    .clEnqueueNDRangeKernel = OclpTermEnqueueNDRangeKernel,
    .clEnqueueTask = OclpTermEnqueueTask,
    .clEnqueueNativeKernel = OclpTermEnqueueNativeKernel,
    .clEnqueueMarker = OclpTermEnqueueMarker,
    .clEnqueueWaitForEvents = OclpTermEnqueueWaitForEvents,
    .clEnqueueBarrier = OclpTermEnqueueBarrier,
    .clGetExtensionFunctionAddress = OclpTermGetExtensionFunctionAddress,
    .clCreateFromGLBuffer = OclpTermCreateFromGLBuffer,
    .clCreateFromGLTexture2D = OclpTermCreateFromGLTexture2D,
    .clCreateFromGLTexture3D = OclpTermCreateFromGLTexture3D,
    .clCreateFromGLRenderbuffer = OclpTermCreateFromGLRenderbuffer,
    .clGetGLObjectInfo = OclpTermGetGLObjectInfo,
    .clGetGLTextureInfo = OclpTermGetGLTextureInfo,
    .clEnqueueAcquireGLObjects = OclpTermEnqueueAcquireGLObjects,
    .clEnqueueReleaseGLObjects = OclpTermEnqueueReleaseGLObjects,
    .clGetGLContextInfoKHR = OclpTermGetGLContextInfoKHR,
    .clGetDeviceIDsFromD3D10KHR = OclpTermGetDeviceIDsFromD3D10KHR,
    .clCreateFromD3D10BufferKHR = OclpTermCreateFromD3D10BufferKHR,
    .clCreateFromD3D10Texture2DKHR = OclpTermCreateFromD3D10Texture2DKHR,
    .clCreateFromD3D10Texture3DKHR = OclpTermCreateFromD3D10Texture3DKHR,
    .clEnqueueAcquireD3D10ObjectsKHR = OclpTermEnqueueAcquireD3D10ObjectsKHR,
    .clEnqueueReleaseD3D10ObjectsKHR = OclpTermEnqueueReleaseD3D10ObjectsKHR,
    .clSetEventCallback = OclpTermSetEventCallback,
    .clCreateSubBuffer = OclpTermCreateSubBuffer,
    .clSetMemObjectDestructorCallback = OclpTermSetMemObjectDestructorCallback,
    .clCreateUserEvent = OclpTermCreateUserEvent,
    .clSetUserEventStatus = OclpTermSetUserEventStatus,
    .clEnqueueReadBufferRect = OclpTermEnqueueReadBufferRect,
    .clEnqueueWriteBufferRect = OclpTermEnqueueWriteBufferRect,
    .clEnqueueCopyBufferRect = OclpTermEnqueueCopyBufferRect,
    .clCreateSubDevicesEXT = OclpTermCreateSubDevicesEXT,
    .clRetainDeviceEXT = OclpTermRetainDeviceEXT,
    .clReleaseDeviceEXT = OclpTermReleaseDeviceEXT,
    .clCreateEventFromGLsyncKHR = OclpTermCreateEventFromGLsyncKHR,
    .clCreateSubDevices = OclpTermCreateSubDevices,
    .clRetainDevice = OclpTermRetainDevice,
    .clReleaseDevice = OclpTermReleaseDevice,
    .clCreateImage = OclpTermCreateImage,
    .clCreateProgramWithBuiltInKernels = OclpTermCreateProgramWithBuiltInKernels,
    .clCompileProgram = OclpTermCompileProgram,
    .clLinkProgram = OclpTermLinkProgram,
    .clUnloadPlatformCompiler = OclpTermUnloadPlatformCompiler,
    .clGetKernelArgInfo = OclpTermGetKernelArgInfo,
    .clEnqueueFillBuffer = OclpTermEnqueueFillBuffer,
    .clEnqueueFillImage = OclpTermEnqueueFillImage,
    .clEnqueueMigrateMemObjects = OclpTermEnqueueMigrateMemObjects,
    .clEnqueueMarkerWithWaitList = OclpTermEnqueueMarkerWithWaitList,
    .clEnqueueBarrierWithWaitList = OclpTermEnqueueBarrierWithWaitList,
    .clGetExtensionFunctionAddressForPlatform = OclpTermGetExtensionFunctionAddressForPlatform,
    .clCreateFromGLTexture = OclpTermCreateFromGLTexture,
    .clGetDeviceIDsFromD3D11KHR = OclpTermGetDeviceIDsFromD3D11KHR,
    .clCreateFromD3D11BufferKHR = OclpTermCreateFromD3D11BufferKHR,
    .clCreateFromD3D11Texture2DKHR = OclpTermCreateFromD3D11Texture2DKHR,
    .clCreateFromD3D11Texture3DKHR = OclpTermCreateFromD3D11Texture3DKHR,
    .clCreateFromDX9MediaSurfaceKHR = OclpTermCreateFromDX9MediaSurfaceKHR,
    .clEnqueueAcquireD3D11ObjectsKHR = OclpTermEnqueueAcquireD3D11ObjectsKHR,
    .clEnqueueReleaseD3D11ObjectsKHR = OclpTermEnqueueReleaseD3D11ObjectsKHR,
    .clGetDeviceIDsFromDX9MediaAdapterKHR = OclpTermGetDeviceIDsFromDX9MediaAdapterKHR,
    .clEnqueueAcquireDX9MediaSurfacesKHR = OclpTermEnqueueAcquireDX9MediaSurfacesKHR,
    .clEnqueueReleaseDX9MediaSurfacesKHR = OclpTermEnqueueReleaseDX9MediaSurfacesKHR,
    .clCreateFromEGLImageKHR = OclpTermCreateFromEGLImageKHR,
    .clEnqueueAcquireEGLObjectsKHR = OclpTermEnqueueAcquireEGLObjectsKHR,
    .clEnqueueReleaseEGLObjectsKHR = OclpTermEnqueueReleaseEGLObjectsKHR,
    .clCreateEventFromEGLSyncKHR = OclpTermCreateEventFromEGLSyncKHR,
    .clCreateCommandQueueWithProperties = OclpTermCreateCommandQueueWithProperties,
    .clCreatePipe = OclpTermCreatePipe,
    .clGetPipeInfo = OclpTermGetPipeInfo,
    .clSVMAlloc = OclpTermSVMAlloc,
    .clSVMFree = OclpTermSVMFree,
    .clEnqueueSVMFree = OclpTermEnqueueSVMFree,
    .clEnqueueSVMMemcpy = OclpTermEnqueueSVMMemcpy,
    .clEnqueueSVMMemFill = OclpTermEnqueueSVMMemFill,
    .clEnqueueSVMMap = OclpTermEnqueueSVMMap,
    .clEnqueueSVMUnmap = OclpTermEnqueueSVMUnmap,
    .clCreateSamplerWithProperties = OclpTermCreateSamplerWithProperties,
    .clSetKernelArgSVMPointer = OclpTermSetKernelArgSVMPointer,
    .clSetKernelExecInfo = OclpTermSetKernelExecInfo,
    .clGetKernelSubGroupInfoKHR = OclpTermGetKernelSubGroupInfoKHR,
    .clCloneKernel = OclpTermCloneKernel,
    .clCreateProgramWithIL = OclpTermCreateProgramWithIL,
    .clEnqueueSVMMigrateMem = OclpTermEnqueueSVMMigrateMem,
    .clGetDeviceAndHostTimer = OclpTermGetDeviceAndHostTimer,
    .clGetHostTimer = OclpTermGetHostTimer,
    .clGetKernelSubGroupInfo = OclpTermGetKernelSubGroupInfo,
    .clSetDefaultDeviceCommandQueue = OclpTermSetDefaultDeviceCommandQueue,
    .clSetProgramReleaseCallback = OclpTermSetProgramReleaseCallback,
    .clSetProgramSpecializationConstant = OclpTermSetProgramSpecializationConstant,
    .clCreateBufferWithProperties = OclpTermCreateBufferWithProperties,
    .clCreateImageWithProperties = OclpTermCreateImageWithProperties,
    .clSetContextDestructorCallback = OclpTermSetContextDestructorCallback
};
