#pragma once

// The PyroWave library's entry points, for the decoder implementations only. Includes
// the Vulkan headers, which the decoders' own headers keep out of everything else.

#include <vulkan/vulkan_core.h>

#ifndef VK_API_VERSION_1_4
// Vulkan headers before 1.4 only have the KHR names, which pyrowave.h doesn't use
typedef VkQueueGlobalPriorityKHR VkQueueGlobalPriority;
#define VK_QUEUE_GLOBAL_PRIORITY_MEDIUM VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR
#define VK_QUEUE_GLOBAL_PRIORITY_HIGH VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR
#endif

#include "pyrowave.h"

// Entry points of the PyroWave library, resolved at runtime
struct PyrowaveApi
{
    decltype(&::pyrowave_get_api_version) getApiVersion;
    decltype(&::pyrowave_create_device_by_compat2) createDeviceByCompat2;
    decltype(&::pyrowave_device_destroy) deviceDestroy;
    decltype(&::pyrowave_device_get_vk_device_handles) deviceGetVkDeviceHandles; // May be null
    decltype(&::pyrowave_sync_object_create) syncObjectCreate;
    decltype(&::pyrowave_sync_object_get_semaphore) syncObjectGetSemaphore;
    decltype(&::pyrowave_sync_object_export_handle) syncObjectExportHandle;
    decltype(&::pyrowave_sync_object_cpu_wait) syncObjectCpuWait;
    decltype(&::pyrowave_sync_object_cpu_signal) syncObjectCpuSignal;
    decltype(&::pyrowave_sync_object_destroy) syncObjectDestroy;
    decltype(&::pyrowave_image_create) imageCreate;
    decltype(&::pyrowave_image_get_image_view) imageGetImageView;
    decltype(&::pyrowave_image_destroy) imageDestroy;
    decltype(&::pyrowave_decoder_create) decoderCreate;
    decltype(&::pyrowave_decoder_clear) decoderClear;
    decltype(&::pyrowave_decoder_push_packet) decoderPushPacket;
    decltype(&::pyrowave_decoder_decode_is_ready) decoderDecodeIsReady;
    decltype(&::pyrowave_decoder_decode_is_ready_with_sideband) decoderDecodeIsReadyWithSideband; // May be null
    decltype(&::pyrowave_decoder_decode_gpu_buffer) decoderDecodeGpuBuffer;
    decltype(&::pyrowave_decoder_destroy) decoderDestroy;
};

// The PyroWave objects every implementation has. Each keeps its images and sync
// objects itself.
struct PyrowaveHandles
{
    pyrowave_device device = nullptr;
    pyrowave_decoder decoder = nullptr;
};

// Loads the library the first time, and returns null if it can't be used
const PyrowaveApi* loadPyrowaveApi();
