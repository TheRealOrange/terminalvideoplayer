#ifdef HAVE_OPENCL

#include "opencl_proc.h"
#include "generated/kernel_source.h"
#include <iostream>
#include <cmath>
#define CHANGE_THRESHOLD 15
#define DITHERING_DECAY 0.7f

OpenCLProc::OpenCLProc() {
}

OpenCLProc::~OpenCLProc() {
    cleanup();
}

void OpenCLProc::cleanup() {
    if (d_frame) clReleaseMemObject(d_frame);
    if (d_old_frame) clReleaseMemObject(d_old_frame);
    if (d_output_frame) clReleaseMemObject(d_output_frame);
    if (d_char_indices) clReleaseMemObject(d_char_indices);
    if (d_fg_colors) clReleaseMemObject(d_fg_colors);
    if (d_bg_colors) clReleaseMemObject(d_bg_colors);
    if (d_needs_update) clReleaseMemObject(d_needs_update);
    if (d_pixelmap) clReleaseMemObject(d_pixelmap);
    if (kernel_process) clReleaseKernel(kernel_process);
    if (program) clReleaseProgram(program);
    if (queue) clReleaseCommandQueue(queue);
    if (context) clReleaseContext(context);
}

std::string OpenCLProc::getKernelSource() {
    return embedded::kernel_source;
}

bool OpenCLProc::initialize() {
#include "pixelmap.h"  // Include to access the pixelmap data
    cl_int err;
    cl_platform_id platform;

    // Get platform
    err = clGetPlatformIDs(1, &platform, nullptr);
    if (err != CL_SUCCESS) {
        std::cerr << "Failed to get OpenCL platform" << std::endl;
        return false;
    }

    // Get GPU device
    err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &device, nullptr);
    if (err != CL_SUCCESS) {
        // Try CPU as fallback
        err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_CPU, 1, &device, nullptr);
        if (err != CL_SUCCESS) {
            std::cerr << "Failed to get OpenCL device" << std::endl;
            return false;
        }
    }

    // Get device name
    char device_name_buffer[256];
    err = clGetDeviceInfo(device, CL_DEVICE_NAME, sizeof(device_name_buffer),
                          device_name_buffer, nullptr);
    if (err == CL_SUCCESS) {
        device_name = std::string(device_name_buffer);
    } else {
        device_name = "Unknown Device";
    }

    // Create context
    context = clCreateContext(nullptr, 1, &device, nullptr, nullptr, &err);
    if (err != CL_SUCCESS) {
        std::cerr << "Failed to create OpenCL context" << std::endl;
        return false;
    }

    // Create command queue
    queue = clCreateCommandQueue(context, device, 0, &err);
    if (err != CL_SUCCESS) {
        std::cerr << "Failed to create command queue" << std::endl;
        return false;
    }

    const int char_y = CHAR_Y;
    const int char_x = CHAR_X;
    const int diff_cases = DIFF_CASES;

    // Create kernel source with injected constants
    std::string kernel_src =
            "#define CHAR_Y " + std::to_string(char_y) + "\n"
            "#define CHAR_X " + std::to_string(char_x) + "\n"
            "#define DIFF_CASES " + std::to_string(diff_cases) + "\n\n"
            + getKernelSource();

    const char *src_ptr = kernel_src.c_str();
    size_t src_len = kernel_src.length();

    program = clCreateProgramWithSource(context, 1, &src_ptr, &src_len, &err);
    if (err != CL_SUCCESS) {
        std::cerr << "Failed to create program" << std::endl;
        return false;
    }

    // Build program
    err = clBuildProgram(program, 1, &device, nullptr, nullptr, nullptr);
    if (err != CL_SUCCESS) {
        char build_log[16384];
        clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG,
                              sizeof(build_log), build_log, nullptr);
        std::cerr << "Failed to build program:\n" << build_log << std::endl;
        return false;
    }

    // Create kernel
    kernel_process = clCreateKernel(program, "process_characters", &err);
    if (err != CL_SUCCESS) {
        std::cerr << "Failed to create kernel" << std::endl;
        return false;
    }

    // Create and upload pixelmap buffer
    // flatten 2D array
    std::vector<int> flat_pixelmap(DIFF_CASES * CHAR_Y * CHAR_X);
    for (int i = 0; i < DIFF_CASES; i++) {
        for (int j = 0; j < CHAR_Y * CHAR_X; j++) {
            flat_pixelmap[i * CHAR_Y * CHAR_X + j] = pixelmap[i][j];
        }
    }

    // Create buffer and copy data
    d_pixelmap = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                flat_pixelmap.size() * sizeof(int),
                                flat_pixelmap.data(), &err);
    if (err != CL_SUCCESS) {
        std::cerr << "Failed to create pixelmap buffer" << std::endl;
        return false;
    }


    initialized = true;
    return true;
}

bool OpenCLProc::createBuffers(size_t frame_size, size_t grid_size) {
    if (current_buffer_size == frame_size && current_grid_size == grid_size) return true;

    cl_int err;

    // Release old buffers
    if (d_frame) clReleaseMemObject(d_frame);
    if (d_old_frame) clReleaseMemObject(d_old_frame);
    if (d_output_frame) clReleaseMemObject(d_output_frame);
    if (d_char_indices) clReleaseMemObject(d_char_indices);
    if (d_fg_colors) clReleaseMemObject(d_fg_colors);
    if (d_bg_colors) clReleaseMemObject(d_bg_colors);
    if (d_needs_update) clReleaseMemObject(d_needs_update);
    d_frame = nullptr;
    d_old_frame = nullptr;
    d_output_frame = nullptr;
    d_char_indices = nullptr;
    d_fg_colors = nullptr;
    d_bg_colors = nullptr;
    d_needs_update = nullptr;

    // Create new buffers
    d_frame = clCreateBuffer(context, CL_MEM_READ_ONLY, frame_size, nullptr, &err);
    if (err != CL_SUCCESS) return false;

    d_old_frame = clCreateBuffer(context, CL_MEM_READ_ONLY, frame_size, nullptr, &err);
    if (err != CL_SUCCESS) return false;

    d_output_frame = clCreateBuffer(context, CL_MEM_WRITE_ONLY, frame_size, nullptr, &err);
    if (err != CL_SUCCESS) return false;

    d_char_indices = clCreateBuffer(context, CL_MEM_WRITE_ONLY,
                                    grid_size * sizeof(int), nullptr, &err);
    if (err != CL_SUCCESS) return false;

    d_fg_colors = clCreateBuffer(context, CL_MEM_WRITE_ONLY,
                                 grid_size * sizeof(int), nullptr, &err);
    if (err != CL_SUCCESS) return false;

    d_bg_colors = clCreateBuffer(context, CL_MEM_WRITE_ONLY,
                                 grid_size * sizeof(int), nullptr, &err);
    if (err != CL_SUCCESS) return false;

    d_needs_update = clCreateBuffer(context, CL_MEM_WRITE_ONLY,
                                    grid_size * sizeof(int), nullptr, &err);
    if (err != CL_SUCCESS) return false;

    current_buffer_size = frame_size;
    current_grid_size = grid_size;
    return true;
}

void OpenCLProc::processFrame(
    const char *frame,
    const char *old_frame,
    char *output_frame,
    int width,
    int height,
    int char_width,
    int char_height,
    int diffthreshold,
    bool refresh,
    int *char_indices,
    int *fg_colors,
    int *bg_colors,
    bool *needs_update
) {
    if (!initialized) return;

    size_t frame_size = width * height * 3;
    size_t grid_size = char_width * char_height;

    if (!createBuffers(frame_size, grid_size)) {
        std::cerr << "Failed to create buffers" << std::endl;
        return;
    }

    cl_int err;

    // Upload data to GPU
    err = clEnqueueWriteBuffer(queue, d_frame, CL_FALSE, 0, frame_size,
                               frame, 0, nullptr, nullptr);
    err |= clEnqueueWriteBuffer(queue, d_old_frame, CL_FALSE, 0, frame_size,
                                old_frame, 0, nullptr, nullptr);

    if (err != CL_SUCCESS) {
        std::cerr << "Failed to upload data to GPU" << std::endl;
        return;
    }

    // Set kernel arguments
    int refresh_int = refresh ? 1 : 0;
    clSetKernelArg(kernel_process, 0, sizeof(cl_mem), &d_frame);
    clSetKernelArg(kernel_process, 1, sizeof(cl_mem), &d_old_frame);
    clSetKernelArg(kernel_process, 2, sizeof(cl_mem), &d_output_frame);
    clSetKernelArg(kernel_process, 3, sizeof(cl_mem), &d_char_indices);
    clSetKernelArg(kernel_process, 4, sizeof(cl_mem), &d_fg_colors);
    clSetKernelArg(kernel_process, 5, sizeof(cl_mem), &d_bg_colors);
    clSetKernelArg(kernel_process, 6, sizeof(cl_mem), &d_needs_update);
    clSetKernelArg(kernel_process, 7, sizeof(int), &width);
    clSetKernelArg(kernel_process, 8, sizeof(int), &height);
    clSetKernelArg(kernel_process, 9, sizeof(int), &char_width);
    clSetKernelArg(kernel_process, 10, sizeof(int), &char_height);
    clSetKernelArg(kernel_process, 11, sizeof(int), &diffthreshold);
    clSetKernelArg(kernel_process, 12, sizeof(int), &refresh_int);
    clSetKernelArg(kernel_process, 13, sizeof(cl_mem), &d_pixelmap);

    // Execute kernel
    size_t global_work_size[2] = {(size_t) char_width, (size_t) char_height};
    err = clEnqueueNDRangeKernel(queue, kernel_process, 2, nullptr,
                                 global_work_size, nullptr, 0, nullptr, nullptr);

    if (err != CL_SUCCESS) {
        std::cerr << "Failed to execute kernel: " << err << std::endl;
        return;
    }

    // Download results
    err = clEnqueueReadBuffer(queue, d_output_frame, CL_FALSE, 0, frame_size,
                              output_frame, 0, nullptr, nullptr);
    err |= clEnqueueReadBuffer(queue, d_char_indices, CL_FALSE, 0,
                               grid_size * sizeof(int),
                               char_indices, 0, nullptr, nullptr);
    err |= clEnqueueReadBuffer(queue, d_fg_colors, CL_FALSE, 0,
                               grid_size * sizeof(int),
                               fg_colors, 0, nullptr, nullptr);
    err |= clEnqueueReadBuffer(queue, d_bg_colors, CL_FALSE, 0,
                               grid_size * sizeof(int),
                               bg_colors, 0, nullptr, nullptr);
    err |= clEnqueueReadBuffer(queue, d_needs_update, CL_TRUE, 0,
                               grid_size * sizeof(bool),
                               (bool *) needs_update, 0, nullptr, nullptr);

    if (err != CL_SUCCESS) {
        std::cerr << "Failed to download results from GPU" << std::endl;
        return;
    }

    clFinish(queue);
}

#endif // HAVE_OPENCL
