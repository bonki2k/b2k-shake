// Minimal OpenCL declarations + runtime loader (OpenCL.dll ships with GPU drivers),
// so no OpenCL SDK is needed to build.
#pragma once
#include <cstddef>
#include <cstdint>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef int32_t cl_int;
typedef uint32_t cl_uint;
typedef uint64_t cl_ulong;
typedef cl_ulong cl_bitfield;
typedef cl_bitfield cl_mem_flags;
typedef cl_bitfield cl_device_type;
typedef cl_bitfield cl_command_queue_properties;
typedef intptr_t cl_context_properties;
typedef struct _cl_platform_id* cl_platform_id;
typedef struct _cl_device_id* cl_device_id;
typedef struct _cl_context* cl_context;
typedef struct _cl_command_queue* cl_command_queue;
typedef struct _cl_mem* cl_mem;
typedef struct _cl_program* cl_program;
typedef struct _cl_kernel* cl_kernel;
typedef struct _cl_event* cl_event;

struct cl_image_format { cl_uint image_channel_order, image_channel_data_type; };
struct cl_image_desc {
    cl_uint image_type;
    size_t image_width, image_height, image_depth, image_array_size, image_row_pitch, image_slice_pitch;
    cl_uint num_mip_levels, num_samples;
    cl_mem buffer;
};

#define CL_SUCCESS 0
#define CL_DEVICE_TYPE_GPU (1 << 2)
#define CL_QUEUE_CONTEXT 0x1090
#define CL_QUEUE_DEVICE 0x1091
#define CL_MEM_READ_WRITE (1 << 0)
#define CL_MEM_READ_ONLY (1 << 2)
#define CL_MEM_COPY_HOST_PTR (1 << 5)
#define CL_PROGRAM_BUILD_LOG 0x1183
#define CL_RGBA 0x10B5
#define CL_FLOAT 0x10DE
#define CL_MEM_OBJECT_IMAGE2D 0x10F1
#define CL_IMAGE_WIDTH 0x1114
#define CL_IMAGE_HEIGHT 0x1115

#define CLAPI __stdcall
struct ClApi {
    cl_int(CLAPI* GetPlatformIDs)(cl_uint, cl_platform_id*, cl_uint*);
    cl_int(CLAPI* GetDeviceIDs)(cl_platform_id, cl_device_type, cl_uint, cl_device_id*, cl_uint*);
    cl_context(CLAPI* CreateContext)(const cl_context_properties*, cl_uint, const cl_device_id*, void*, void*, cl_int*);
    cl_command_queue(CLAPI* CreateCommandQueue)(cl_context, cl_device_id, cl_command_queue_properties, cl_int*);
    cl_int(CLAPI* GetCommandQueueInfo)(cl_command_queue, cl_uint, size_t, void*, size_t*);
    cl_program(CLAPI* CreateProgramWithSource)(cl_context, cl_uint, const char**, const size_t*, cl_int*);
    cl_int(CLAPI* BuildProgram)(cl_program, cl_uint, const cl_device_id*, const char*, void*, void*);
    cl_int(CLAPI* GetProgramBuildInfo)(cl_program, cl_device_id, cl_uint, size_t, void*, size_t*);
    cl_kernel(CLAPI* CreateKernel)(cl_program, const char*, cl_int*);
    cl_int(CLAPI* SetKernelArg)(cl_kernel, cl_uint, size_t, const void*);
    cl_int(CLAPI* EnqueueNDRangeKernel)(cl_command_queue, cl_kernel, cl_uint, const size_t*, const size_t*, const size_t*, cl_uint, const cl_event*, cl_event*);
    cl_mem(CLAPI* CreateBuffer)(cl_context, cl_mem_flags, size_t, void*, cl_int*);
    cl_mem(CLAPI* CreateImage)(cl_context, cl_mem_flags, const cl_image_format*, const cl_image_desc*, void*, cl_int*);
    cl_int(CLAPI* EnqueueReadImage)(cl_command_queue, cl_mem, cl_uint, const size_t*, const size_t*, size_t, size_t, void*, cl_uint, const cl_event*, cl_event*);
    cl_int(CLAPI* GetImageInfo)(cl_mem, cl_uint, size_t, void*, size_t*);
    cl_int(CLAPI* ReleaseMemObject)(cl_mem);
    cl_int(CLAPI* Finish)(cl_command_queue);
    bool ok = false;

    bool load() {
        if (ok) return true;
        HMODULE m = LoadLibraryA("OpenCL.dll");
        if (!m) return false;
#define CLL(name) if (!(*(FARPROC*)&name = GetProcAddress(m, "cl" #name))) return false;
        CLL(GetPlatformIDs) CLL(GetDeviceIDs) CLL(CreateContext) CLL(CreateCommandQueue) CLL(GetCommandQueueInfo)
        CLL(CreateProgramWithSource) CLL(BuildProgram) CLL(GetProgramBuildInfo) CLL(CreateKernel) CLL(SetKernelArg)
        CLL(EnqueueNDRangeKernel) CLL(CreateBuffer) CLL(CreateImage) CLL(EnqueueReadImage) CLL(GetImageInfo)
        CLL(ReleaseMemObject) CLL(Finish)
#undef CLL
        ok = true;
        return true;
    }
};
