cmake_minimum_required(VERSION 3.16)
project(decode_latency CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(decode_latency decode_latency.cpp)

if(WIN32)
    # An FFmpeg 8 "shared" dev package with include/, lib/ and bin/, e.g. BtbN's
    # ffmpeg-n8.0-latest-win64-gpl-shared-8.0 (built with --enable-amf).
    set(FFMPEG_DIR "" CACHE PATH "FFmpeg dev package root (contains include/, lib/, bin/)")
    if(NOT FFMPEG_DIR)
        message(FATAL_ERROR "Set FFMPEG_DIR to an FFmpeg 8 shared dev package")
    endif()
    target_include_directories(decode_latency PRIVATE "${FFMPEG_DIR}/include")
    target_link_directories(decode_latency PRIVATE "${FFMPEG_DIR}/lib")
    target_link_libraries(decode_latency PRIVATE avcodec avformat avutil d3d11 d3d12 dxgi winmm)

    # Put FFmpeg's DLLs next to the exe so it runs from the build directory
    file(GLOB FFMPEG_DLLS "${FFMPEG_DIR}/bin/*.dll")
    foreach(dll ${FFMPEG_DLLS})
        add_custom_command(TARGET decode_latency POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:decode_latency>")
    endforeach()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil)
    target_link_libraries(decode_latency PRIVATE PkgConfig::FFMPEG)

    pkg_check_modules(LIBVA IMPORTED_TARGET libva)
    if(LIBVA_FOUND)
        target_link_libraries(decode_latency PRIVATE PkgConfig::LIBVA)
        target_compile_definitions(decode_latency PRIVATE HARNESS_HAVE_VAAPI)
    endif()
endif()

# Vulkan headers only; vkWaitSemaphores is looked up through FFmpeg's device.
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    target_include_directories(decode_latency PRIVATE ${Vulkan_INCLUDE_DIRS})
    target_compile_definitions(decode_latency PRIVATE HARNESS_HAVE_VULKAN)
endif()

# AMF headers (header-only), so AMF frames can be waited on: the directory that contains
# AMF/core/Surface.h. GPUOpen's AMF repo has them in amf/public/include, which needs to
# be reachable as AMF/..., e.g. copy that folder to <dir>/AMF and set AMF_INCLUDE_DIR=<dir>.
set(AMF_INCLUDE_DIR "" CACHE PATH "Directory containing AMF/core/Surface.h")
if(AMF_INCLUDE_DIR)
    target_include_directories(decode_latency PRIVATE "${AMF_INCLUDE_DIR}")
    target_compile_definitions(decode_latency PRIVATE HARNESS_HAVE_AMF)
endif()

message(STATUS "decode_latency: Vulkan=${Vulkan_FOUND} VAAPI=${LIBVA_FOUND} AMF=${AMF_INCLUDE_DIR}")
