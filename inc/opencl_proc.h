//
// Created by Lin Yicheng on 5/10/25.
//

#ifndef TVP_OPENCL_PROC_H
#define TVP_OPENCL_PROC_H

#ifdef HAVE_OPENCL

#ifdef __APPLE__
#include <OpenCL/opencl.h>
#else
#include <CL/cl.h>
#endif

#include <string>
#include <vector>

class OpenCLProc {
public:
  OpenCLProc();
  ~OpenCLProc();

  bool initialize();
  bool isInitialized() const { return initialized; }

  // Process entire frame on GPU
  void processFrame(
      const char* frame,
      const char* old_frame,
      char* output_frame,
      float* error_buffer,
      int width,
      int height,
      int char_width,
      int char_height,
      int diffthreshold,
      bool refresh,
      // Output arrays
      int* char_indices,    // which character to use
      int* fg_colors,       // RGB foreground colors (packed)
      int* bg_colors,       // RGB background colors (packed)
      bool* needs_update    // which characters need updating
  );

private:
  bool initialized = false;
  cl_context context = nullptr;
  cl_command_queue queue = nullptr;
  cl_program program = nullptr;
  cl_kernel kernel_process = nullptr;
  cl_device_id device = nullptr;

  // Device memory buffers
  cl_mem d_frame = nullptr;
  cl_mem d_old_frame = nullptr;
  cl_mem d_output_frame = nullptr;
  cl_mem d_error_buffer = nullptr;
  cl_mem d_char_indices = nullptr;
  cl_mem d_fg_colors = nullptr;
  cl_mem d_bg_colors = nullptr;
  cl_mem d_needs_update = nullptr;
  cl_mem d_pixelmap = nullptr;

  size_t current_buffer_size = 0;

  void cleanup();
  bool createBuffers(size_t frame_size, size_t grid_size);
  std::string getKernelSource();
};

#endif // HAVE_OPENCL
#endif //TVP_OPENCL_PROC_H