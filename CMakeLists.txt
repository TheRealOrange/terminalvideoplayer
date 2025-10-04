cmake_minimum_required(VERSION 3.15)
project(tvp VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ffmpeg libraries
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED
        libavformat
        libavcodec
        libavutil
        libswscale
)

set(SOURCES src/main.cpp src/video.cpp)

add_executable(tvp ${SOURCES})
target_include_directories(tvp PRIVATE ${CMAKE_SOURCE_DIR}/inc ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(tvp PRIVATE ${FFMPEG_LIBRARIES})
target_link_directories(tvp PRIVATE ${FFMPEG_LIBRARY_DIRS})

target_compile_options(tvp PRIVATE -O3 -Wall -Wextra)


# Platform-specific settings
if(WIN32)
    # none for now
elseif(APPLE)
    # macOS-specific
    find_library(COREFOUNDATION_LIBRARY CoreFoundation)
    if(COREFOUNDATION_LIBRARY)
        target_link_libraries(tvp PRIVATE ${COREFOUNDATION_LIBRARY})
    endif()
elseif(UNIX)
    # Linux-specific
    target_link_libraries(tvp PRIVATE pthread)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "FFmpeg libraries: ${FFMPEG_LIBRARIES}")
message(STATUS "FFmpeg include dirs: ${FFMPEG_INCLUDE_DIRS}")