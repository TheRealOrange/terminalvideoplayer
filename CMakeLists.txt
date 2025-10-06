cmake_minimum_required(VERSION 3.15)
project(tvp VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Check if LTO is supported
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT error)

# optional opencl if found
find_package(OpenCL)
if (OpenCL_FOUND)
    message(STATUS "opencl found: gpu acceleration enabled")
    add_compile_definitions(HAVE_OPENCL)
    set(OPENCL_SOURCES src/opencl_proc.cpp)

    # Include the embed function
    include(${CMAKE_CURRENT_SOURCE_DIR}/embed_file.cmake)

    # Embed OpenCL kernel file
    embed_opencl_kernel(
            INPUT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/process_frame.cl"
            OUTPUT_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/inc/generated/kernel_source.h"
            VARIABLE_NAME "kernel_source"
    )

    # Track the kernel file so CMake reconfigures when it changes
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/src/process_frame.cl"
    )
else ()
    message(STATUS "opencl not found: building without GPU acceleration")
endif ()

# ffmpeg libraries
if (WIN32)
    # windows
    # use find_package (vcpkg)
    find_package(SDL2 CONFIG REQUIRED)
    find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h)
    find_library(AVCODEC_LIBRARY avcodec)
    find_library(AVFORMAT_LIBRARY avformat)
    find_library(AVUTIL_LIBRARY avutil)
    find_library(SWSCALE_LIBRARY swscale)
    find_library(SWRESAMPLE_LIBRARY swresample)

    set(FFMPEG_INCLUDE_DIRS ${AVCODEC_INCLUDE_DIR})
    set(FFMPEG_LIBRARIES ${AVCODEC_LIBRARY} ${AVFORMAT_LIBRARY}
            ${AVUTIL_LIBRARY} ${SWSCALE_LIBRARY}
            ${SWRESAMPLE_LIBRARY})
else ()
    # linux or macOS
    # use pkgconfig to fing ffmpeg and sdl
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED
            libavformat
            libavcodec
            libavutil
            libswscale
            libswresample
    )
    pkg_check_modules(SDL2 REQUIRED sdl2)
endif ()
set(SOURCES src/main.cpp src/video.cpp ${OPENCL_SOURCES})

add_executable(tvp ${SOURCES})
target_include_directories(tvp PRIVATE
        ${CMAKE_SOURCE_DIR}/inc
        ${FFMPEG_INCLUDE_DIRS}
        ${SDL2_INCLUDE_DIRS}
)
target_link_libraries(tvp PRIVATE ${FFMPEG_LIBRARIES} ${SDL2_LIBRARIES})
target_link_directories(tvp PRIVATE ${FFMPEG_LIBRARY_DIRS} ${SDL2_LIBRARY_DIRS})

if (WIN32)
    # Windows system libraries required by static FFmpeg
    target_link_libraries(tvp PRIVATE
            ws2_32      # Winsock2 - networking functions
            secur32     # Security Support Provider Interface
            bcrypt      # Cryptography functions
            mfplat      # Media Foundation Platform
            mfuuid      # Media Foundation UUIDs
            strmiids    # DirectShow
    )
    if (MINGW)
        target_link_options(tvp PRIVATE -static-libgcc -static-libstdc++ -static)
    endif ()
endif ()

if (OpenCL_FOUND)
    target_include_directories(tvp PRIVATE ${OpenCL_INCLUDE_DIRS})
    target_link_libraries(tvp PRIVATE ${OpenCL_LIBRARIES})
endif ()

target_compile_options(tvp PRIVATE -O3 -Wall -Wextra -ffast-math -march=native)

if (ipo_supported)
    set_target_properties(tvp PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
    message(STATUS "ipo/lto enabled for tvp")
else ()
    message(STATUS "ipo/lto not supported: ${error}")
endif ()

# Platform-specific settings
if (WIN32)
    # none for now
elseif (APPLE)
    # macOS-specific
    find_library(COREFOUNDATION_LIBRARY CoreFoundation)
    if (COREFOUNDATION_LIBRARY)
        target_link_libraries(tvp PRIVATE ${COREFOUNDATION_LIBRARY})
    endif ()
    if (OpenCL_FOUND)
        target_link_libraries(tvp PRIVATE "-framework OpenCL")
    endif ()
elseif (UNIX)
    # Linux-specific
    target_link_libraries(tvp PRIVATE pthread)
endif ()

message(STATUS "build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "ffmpeg libraries: ${FFMPEG_LIBRARIES}")
message(STATUS "ffmpeg include dirs: ${FFMPEG_INCLUDE_DIRS}")
message(STATUS "sdl2 libraries: ${SDL2_LIBRARIES}")
message(STATUS "sdl2 include dirs: ${SDL2_INCLUDE_DIRS}")
if (OpenCL_FOUND)
    message(STATUS "opencl libraries: ${OpenCL_LIBRARIES}")
    message(STATUS "opencl include dirs: ${OpenCL_INCLUDE_DIRS}")
endif ()