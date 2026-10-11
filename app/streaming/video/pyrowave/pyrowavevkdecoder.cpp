#include "pyrowavevkdecoder.h"
#include "pyrowaveapi.h"

#include "streaming/video/ffmpeg-renderers/pacer/pacer.h"

// Implementation in plvk_c.c
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

#include <array>
#include <cstring>
#include <type_traits>

#include <unistd.h>

extern "C" {
#include <libavutil/buffer.h>
}

// How PyroWave and the renderer use the planes
static const VkImageUsageFlags k_PlaneImageUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

// The pacer can hold on to all of its outstanding frames while we decode another
#define PYROWAVE_VK_SLOTS (PACER_MAX_OUTSTANDING_FRAMES + 2)

// One set of Y, Cb and Cr planes, which a frame refers to while it's in use
struct PyrowaveVkSlot
{
    int index = 0;

    // On the renderer's device
    std::array<VkImage, 3> images = {};
    std::array<VkDeviceMemory, 3> memory = {};
    std::array<pl_tex, 3> textures = {};

    // The same images imported into PyroWave's device
    std::array<pyrowave_image, 3> pyrowaveImages = {};
    pyrowave_gpu_buffers buffers = {};

    // The decoded semaphore's value once PyroWave has decoded into the slot. Written on
    // the decoder thread before the frame is handed over.
    uint64_t decodedValue = 0;

    // The rendered semaphore's value once the renderer is done with the slot, or 0 if
    // it hasn't drawn from it. Written on the render thread before the frame is freed,
    // and read on the decoder thread after it takes the slot from the free list.
    uint64_t renderedValue = 0;
};

struct PyrowaveVkState
{
    pl_vulkan vulkan = nullptr;

    PFN_vkGetPhysicalDeviceProperties2 getPhysicalDeviceProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties getPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements getImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkBindImageMemory bindImageMemory = nullptr;
    PFN_vkGetMemoryFdKHR getMemoryFd = nullptr;
    PFN_vkGetSemaphoreCounterValue getSemaphoreCounterValue = nullptr;
    PFN_vkWaitSemaphores waitSemaphores = nullptr;

    VkPhysicalDeviceMemoryProperties memoryProperties = {};

    // Signalled by PyroWave as it finishes decoding into a slot, and by the renderer as
    // it finishes with one. Both made on the renderer's device and imported into PyroWave's.
    VkSemaphore decodedSemaphore = VK_NULL_HANDLE;
    VkSemaphore renderedSemaphore = VK_NULL_HANDLE;
    pyrowave_sync_object decodedSync = nullptr;
    pyrowave_sync_object renderedSync = nullptr;

    // The last value signalled on each. Only the decoder thread touches the first and
    // only the render thread the second.
    uint64_t decodedValue = 0;
    uint64_t renderedValue = 0;

    // Whether waitForDecode() still waits. Cleared if a wait ever fails.
    bool decodeWaitEnabled = true;

    std::array<PyrowaveVkSlot, PYROWAVE_VK_SLOTS> slotPool; // Not "slots", which Qt defines as a macro
};

PyrowaveVkDecoder::PyrowaveVkDecoder()
    : m_Renderer(nullptr),
      m_State(new PyrowaveVkState()),
      m_LoggedNoFreeSlot(false),
      m_LoggedHoldFailure(false)
{
}

PyrowaveVkDecoder::~PyrowaveVkDecoder()
{
    if (m_Renderer != nullptr) {
        m_Renderer->setFrameSource(nullptr);
    }

    // Waits for PyroWave's device to be done with the planes
    destroyDecoder();

    if (m_State->vulkan != nullptr) {
        // And the renderer's
        pl_gpu_finish(m_State->vulkan->gpu);

        for (PyrowaveVkSlot& slot : m_State->slotPool) {
            destroySlot(&slot);
        }

        if (m_Api != nullptr) {
            if (m_State->decodedSync != nullptr) {
                m_Api->syncObjectDestroy(m_State->decodedSync);
            }
            if (m_State->renderedSync != nullptr) {
                m_Api->syncObjectDestroy(m_State->renderedSync);
            }
        }

        for (VkSemaphore* semaphore : { &m_State->decodedSemaphore, &m_State->renderedSemaphore }) {
            if (*semaphore != VK_NULL_HANDLE) {
                pl_vulkan_sem_destroy(m_State->vulkan->gpu, semaphore);
            }
        }
    }

    delete m_State;
}

bool PyrowaveVkDecoder::initialize(PlVkRenderer* renderer, PDECODER_PARAMETERS params, int colorspace)
{
    if (!initializeStream(params, colorspace)) {
        return false;
    }

    m_State->vulkan = renderer->getVulkan();
    if (m_State->vulkan == nullptr) {
        return false;
    }

    // PyroWave's device and the renderer's share the planes and semaphores as fds
    pl_gpu gpu = m_State->vulkan->gpu;
    if (!(gpu->export_caps.tex & PL_HANDLE_FD) || !(gpu->export_caps.sync & PL_HANDLE_FD)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: the Vulkan driver can't share images and semaphores with another device (VK_KHR_external_memory_fd and VK_KHR_external_semaphore_fd are needed)");
        return false;
    }

    if (!loadVulkanFunctions() || !createPyrowaveDevice() || !createSemaphores()) {
        return false;
    }

    for (int i = 0; i < PYROWAVE_VK_SLOTS; i++) {
        m_State->slotPool[i].index = i;
        if (!createSlot(&m_State->slotPool[i])) {
            return false;
        }
        m_FreeSlots.push_back(i);
    }

    if (!createDecoder()) {
        return false;
    }

    m_Renderer = renderer;
    m_Renderer->setFrameSource(this);

    VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    m_State->getPhysicalDeviceProperties2(m_State->vulkan->phys_device, &props);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PyroWave: decoding %dx%d %s %s on %s",
                m_Width, m_Height,
                m_TenBit ? "10-bit" : "8-bit",
                m_Yuv444 ? "4:4:4" : "4:2:0",
                props.properties.deviceName);
    return true;
}

bool PyrowaveVkDecoder::loadVulkanFunctions()
{
    pl_vulkan vulkan = m_State->vulkan;

    auto getDeviceProcAddr = (PFN_vkGetDeviceProcAddr)vulkan->get_proc_addr(vulkan->instance, "vkGetDeviceProcAddr");
    if (getDeviceProcAddr == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: vkGetDeviceProcAddr() is missing");
        return false;
    }

    bool resolved = true;
    auto resolveInstance = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(vulkan->get_proc_addr(vulkan->instance, name));
        if (fn == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: Vulkan function %s is missing",
                         name);
            resolved = false;
        }
    };
    auto resolveDevice = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(getDeviceProcAddr(vulkan->device, name));
        if (fn == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: Vulkan function %s is missing",
                         name);
            resolved = false;
        }
    };

    resolveInstance(m_State->getPhysicalDeviceProperties2, "vkGetPhysicalDeviceProperties2");
    resolveInstance(m_State->getPhysicalDeviceMemoryProperties, "vkGetPhysicalDeviceMemoryProperties");
    resolveDevice(m_State->createImage, "vkCreateImage");
    resolveDevice(m_State->destroyImage, "vkDestroyImage");
    resolveDevice(m_State->getImageMemoryRequirements, "vkGetImageMemoryRequirements");
    resolveDevice(m_State->allocateMemory, "vkAllocateMemory");
    resolveDevice(m_State->freeMemory, "vkFreeMemory");
    resolveDevice(m_State->bindImageMemory, "vkBindImageMemory");
    resolveDevice(m_State->getMemoryFd, "vkGetMemoryFdKHR");
    resolveDevice(m_State->getSemaphoreCounterValue, "vkGetSemaphoreCounterValue");
    resolveDevice(m_State->waitSemaphores, "vkWaitSemaphores");
    if (!resolved) {
        return false;
    }

    m_State->getPhysicalDeviceMemoryProperties(vulkan->phys_device, &m_State->memoryProperties);
    return true;
}

// PyroWave makes its own Vulkan device on the renderer's GPU, found by its UUIDs
bool PyrowaveVkDecoder::createPyrowaveDevice()
{
    VkPhysicalDeviceIDProperties idProps = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    props.pNext = &idProps;
    m_State->getPhysicalDeviceProperties2(m_State->vulkan->phys_device, &props);

    static_assert(sizeof(idProps.deviceUUID) == sizeof(pyrowave_uuid), "UUID size mismatch");
    pyrowave_uuid deviceUuid, driverUuid;
    memcpy(deviceUuid.uuid, idProps.deviceUUID, sizeof(deviceUuid.uuid));
    memcpy(driverUuid.uuid, idProps.driverUUID, sizeof(driverUuid.uuid));

    // High priority puts PyroWave on an async compute queue. Linux only allows it with
    // CAP_SYS_NICE, and the driver may refuse the device rather than lower it, so
    // that's retried at the normal priority.
    pyrowave_result result = PYROWAVE_ERROR_GENERIC;
    for (VkQueueGlobalPriority priority : { VK_QUEUE_GLOBAL_PRIORITY_HIGH, VK_QUEUE_GLOBAL_PRIORITY_MEDIUM }) {
        result = m_Api->createDeviceByCompat2(props.properties.vendorID, props.properties.deviceID,
                                              &deviceUuid, &driverUuid, nullptr,
                                              priority, &m_Handles->device);
        if (result == PYROWAVE_SUCCESS) {
            return true;
        }
    }

    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                 "PyroWave: couldn't create a Vulkan device for %s (%d). A Vulkan 1.3 driver is required.",
                 props.properties.deviceName,
                 result);
    return false;
}

// Makes the two timeline semaphores on the renderer's device and imports them into
// PyroWave's
bool PyrowaveVkDecoder::createSemaphores()
{
    pl_gpu gpu = m_State->vulkan->gpu;

    struct {
        VkSemaphore* semaphore;
        pyrowave_sync_object* sync;
    } semaphores[] = {
        { &m_State->decodedSemaphore, &m_State->decodedSync },
        { &m_State->renderedSemaphore, &m_State->renderedSync },
    };

    for (const auto& entry : semaphores) {
        pl_handle handle = {};
        handle.fd = -1;

        pl_vulkan_sem_params semParams = {};
        semParams.type = VK_SEMAPHORE_TYPE_TIMELINE;
        semParams.initial_value = 0;
        semParams.export_handle = PL_HANDLE_FD;
        semParams.out_handle = &handle;
        *entry.semaphore = pl_vulkan_sem_create(gpu, &semParams);
        if (*entry.semaphore == VK_NULL_HANDLE) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: couldn't create a shareable timeline semaphore");
            return false;
        }

        pyrowave_sync_object_create_info syncInfo = {};
        syncInfo.device = m_Handles->device;
        syncInfo.external_handle = (pyrowave_os_handle)handle.fd;
        syncInfo.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        syncInfo.semaphore_type = VK_SEMAPHORE_TYPE_TIMELINE;
        pyrowave_result result = m_Api->syncObjectCreate(&syncInfo, entry.sync);
        if (result != PYROWAVE_SUCCESS) {
            // PyroWave only takes the fd over when the import succeeds
            close(handle.fd);
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: couldn't import a timeline semaphore (%d)",
                         result);
            return false;
        }
    }

    return true;
}

bool PyrowaveVkDecoder::createSlot(PyrowaveVkSlot* slot)
{
    for (int i = 0; i < 3; i++) {
        if (!createPlane(slot, i)) {
            return false;
        }
    }

    return true;
}

bool PyrowaveVkDecoder::createPlane(PyrowaveVkSlot* slot, int plane)
{
    VkDevice device = m_State->vulkan->device;
    VkResult vkResult;

    // Cb and Cr are half size for 4:2:0
    bool fullSize = plane == 0 || m_Yuv444;
    int planeWidth = fullSize ? m_Width : m_Width / 2;
    int planeHeight = fullSize ? m_Height : m_Height / 2;
    VkFormat format = m_TenBit ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;

    // PyroWave's device imports this with the same create info, apart from pNext
    VkImageCreateInfo imageCreateInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    imageCreateInfo.format = format;
    imageCreateInfo.extent = { (uint32_t)planeWidth, (uint32_t)planeHeight, 1 };
    imageCreateInfo.mipLevels = 1;
    imageCreateInfo.arrayLayers = 1;
    imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCreateInfo.usage = k_PlaneImageUsage;
    imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkExternalMemoryImageCreateInfo externalInfo = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    externalInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkImageCreateInfo exportableCreateInfo = imageCreateInfo;
    exportableCreateInfo.pNext = &externalInfo;
    vkResult = m_State->createImage(device, &exportableCreateInfo, nullptr, &slot->images[plane]);
    if (vkResult != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: vkCreateImage() failed: %d",
                     vkResult);
        return false;
    }

    VkMemoryRequirements requirements;
    m_State->getImageMemoryRequirements(device, slot->images[plane], &requirements);

    uint32_t memoryType = UINT32_MAX;
    for (uint32_t i = 0; i < m_State->memoryProperties.memoryTypeCount; i++) {
        if ((requirements.memoryTypeBits & (1U << i)) &&
                (m_State->memoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memoryType = i;
            break;
        }
    }
    if (memoryType == UINT32_MAX) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: no device memory for the planes");
        return false;
    }

    // A dedicated allocation, as an importer of an image's memory generally expects
    VkMemoryDedicatedAllocateInfo dedicatedInfo = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    dedicatedInfo.image = slot->images[plane];

    VkExportMemoryAllocateInfo exportInfo = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
    exportInfo.pNext = &dedicatedInfo;
    exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkMemoryAllocateInfo allocateInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocateInfo.pNext = &exportInfo;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = memoryType;
    vkResult = m_State->allocateMemory(device, &allocateInfo, nullptr, &slot->memory[plane]);
    if (vkResult != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: vkAllocateMemory() failed: %d",
                     vkResult);
        return false;
    }

    vkResult = m_State->bindImageMemory(device, slot->images[plane], slot->memory[plane], 0);
    if (vkResult != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: vkBindImageMemory() failed: %d",
                     vkResult);
        return false;
    }

    VkMemoryGetFdInfoKHR fdInfo = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
    fdInfo.memory = slot->memory[plane];
    fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd = -1;
    vkResult = m_State->getMemoryFd(device, &fdInfo, &fd);
    if (vkResult != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: vkGetMemoryFdKHR() failed: %d",
                     vkResult);
        return false;
    }

    pyrowave_image_create_info imageInfo = {};
    imageInfo.device = m_Handles->device;
    imageInfo.external_handle = (pyrowave_os_handle)fd;
    imageInfo.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    imageInfo.image_create_info = &imageCreateInfo;
    pyrowave_result result = m_Api->imageCreate(&imageInfo, &slot->pyrowaveImages[plane]);
    if (result != PYROWAVE_SUCCESS) {
        // PyroWave only takes the fd over when the import succeeds
        close(fd);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: couldn't import plane %d (%d)",
                     plane, result);
        return false;
    }

    result = m_Api->imageGetImageView(slot->pyrowaveImages[plane],
                                      VK_IMAGE_ASPECT_COLOR_BIT,
                                      VK_IMAGE_USAGE_STORAGE_BIT,
                                      &slot->buffers.planes[plane]);
    if (result != PYROWAVE_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: couldn't view plane %d (%d)",
                     plane, result);
        return false;
    }

    // The renderer draws it straight from here. It starts out held by us, until
    // mapFrame() releases it to libplacebo.
    pl_vulkan_wrap_params wrapParams = {};
    wrapParams.image = slot->images[plane];
    wrapParams.width = planeWidth;
    wrapParams.height = planeHeight;
    wrapParams.format = format;
    wrapParams.usage = k_PlaneImageUsage;
    slot->textures[plane] = pl_vulkan_wrap(m_State->vulkan->gpu, &wrapParams);
    if (slot->textures[plane] == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: pl_vulkan_wrap() failed for plane %d",
                     plane);
        return false;
    }

    return true;
}

void PyrowaveVkDecoder::destroySlot(PyrowaveVkSlot* slot)
{
    VkDevice device = m_State->vulkan->device;

    for (int i = 0; i < 3; i++) {
        pl_tex_destroy(m_State->vulkan->gpu, &slot->textures[i]);

        if (slot->pyrowaveImages[i] != nullptr && m_Api != nullptr) {
            m_Api->imageDestroy(slot->pyrowaveImages[i]);
            slot->pyrowaveImages[i] = nullptr;
        }

        if (slot->images[i] != VK_NULL_HANDLE) {
            m_State->destroyImage(device, slot->images[i], nullptr);
            slot->images[i] = VK_NULL_HANDLE;
        }

        if (slot->memory[i] != VK_NULL_HANDLE) {
            m_State->freeMemory(device, slot->memory[i], nullptr);
            slot->memory[i] = VK_NULL_HANDLE;
        }
    }
}

// Frees a frame's slot for decoding into again, when the last reference to the frame goes
void PyrowaveVkDecoder::releaseSlot(void* opaque, uint8_t* data)
{
    auto me = (PyrowaveVkDecoder*)opaque;
    auto slot = (PyrowaveVkSlot*)data;

    std::lock_guard<std::mutex> lock(me->m_FreeSlotsLock);
    me->m_FreeSlots.push_back(slot->index);
}

AVFrame* PyrowaveVkDecoder::decodePushedFrame()
{
    PyrowaveVkSlot* slot;
    {
        std::lock_guard<std::mutex> lock(m_FreeSlotsLock);
        if (m_FreeSlots.empty()) {
            slot = nullptr;
        }
        else {
            slot = &m_State->slotPool[m_FreeSlots.back()];
            m_FreeSlots.pop_back();
        }
    }

    // Every slot is held by a frame waiting to be rendered
    if (slot == nullptr) {
        if (!m_LoggedNoFreeSlot) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "PyroWave: no free frame to decode into");
            m_LoggedNoFreeSlot = true;
        }
        return nullptr;
    }

    auto returnSlot = [this, slot]() {
        std::lock_guard<std::mutex> lock(m_FreeSlotsLock);
        m_FreeSlots.push_back(slot->index);
    };

    // The planes' old contents can be discarded, but not before the renderer is done
    // with them. They go to the renderer's device when decoded.
    std::array<pyrowave_gpu_external_reference, 3> acquireRefs, releaseRefs;
    for (int i = 0; i < 3; i++) {
        acquireRefs[i] = { slot->pyrowaveImages[i], VK_QUEUE_FAMILY_IGNORED };
        releaseRefs[i] = { slot->pyrowaveImages[i], VK_QUEUE_FAMILY_EXTERNAL };
    }

    uint64_t decodedValue = m_State->decodedValue + 1;

    pyrowave_gpu_sync_operation acquire = {};
    acquire.images = acquireRefs.data();
    acquire.num_images = acquireRefs.size();
    // Nothing to wait for if the renderer never drew from this slot (PyroWave would
    // take a value of 0 as a binary semaphore)
    if (slot->renderedValue != 0) {
        acquire.sync = { m_Api->syncObjectGetSemaphore(m_State->renderedSync), slot->renderedValue };
    }

    pyrowave_gpu_sync_operation release = {};
    release.images = releaseRefs.data();
    release.num_images = releaseRefs.size();
    release.sync = { m_Api->syncObjectGetSemaphore(m_State->decodedSync), decodedValue };

    pyrowave_result result = m_Api->decoderDecodeGpuBuffer(m_Handles->decoder, &acquire, &release, &slot->buffers);
    if (result != PYROWAVE_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: decoding failed (%d)",
                     result);
        returnSlot();
        return nullptr;
    }

    m_State->decodedValue = decodedValue;
    slot->decodedValue = decodedValue;

    AVFrame* frame = av_frame_alloc();
    if (frame == nullptr) {
        returnSlot();
        return nullptr;
    }

    // The frame holds the slot until it's freed. Its format says how the planes are
    // laid out (PyroWave fills the whole range of the 16-bit planes for 10-bit), but its
    // data is the slot, never pixels. Only the renderer reads it, through mapFrame().
    frame->buf[0] = av_buffer_create((uint8_t*)slot, sizeof(*slot), releaseSlot, this, 0);
    if (frame->buf[0] == nullptr) {
        av_frame_free(&frame);
        returnSlot();
        return nullptr;
    }
    frame->data[0] = (uint8_t*)slot;
    frame->width = m_Width;
    frame->height = m_Height;
    if (m_Yuv444) {
        frame->format = m_TenBit ? AV_PIX_FMT_YUV444P16 : AV_PIX_FMT_YUV444P;
    }
    else {
        frame->format = m_TenBit ? AV_PIX_FMT_YUV420P16 : AV_PIX_FMT_YUV420P;
    }

    return frame;
}

// Hands a frame's planes to libplacebo, which waits on the GPU for PyroWave to finish
// decoding them before it draws them
bool PyrowaveVkDecoder::mapFrame(const AVFrame* frame, pl_frame* mappedFrame)
{
    auto slot = (PyrowaveVkSlot*)frame->data[0];

    // Colorimetry, crop, chroma location, and which plane holds which component
    pl_frame_from_avframe(mappedFrame, frame);
    SDL_assert(mappedFrame->num_planes == 3);

    for (int i = 0; i < 3; i++) {
        pl_vulkan_release_params releaseParams = {};
        releaseParams.tex = slot->textures[i];
        releaseParams.layout = VK_IMAGE_LAYOUT_GENERAL;
        releaseParams.qf = VK_QUEUE_FAMILY_EXTERNAL;
        releaseParams.semaphore = { m_State->decodedSemaphore, slot->decodedValue };
        pl_vulkan_release_ex(m_State->vulkan->gpu, &releaseParams);

        mappedFrame->planes[i].texture = slot->textures[i];
    }

    return true;
}

// Takes a frame's planes back from libplacebo once it's drawn them, signalling the
// rendered semaphore when the GPU is done, which PyroWave waits for before decoding
// into them again
void PyrowaveVkDecoder::unmapFrame(const AVFrame* frame)
{
    auto slot = (PyrowaveVkSlot*)frame->data[0];
    pl_gpu gpu = m_State->vulkan->gpu;

    // Each signal gets its own value. Only one that was actually signalled may be
    // waited for, or PyroWave would wait forever.
    uint64_t heldValue = 0;
    for (int i = 0; i < 3; i++) {
        pl_vulkan_hold_params holdParams = {};
        holdParams.tex = slot->textures[i];
        holdParams.layout = VK_IMAGE_LAYOUT_GENERAL;
        holdParams.qf = VK_QUEUE_FAMILY_EXTERNAL;
        holdParams.semaphore = { m_State->renderedSemaphore, m_State->renderedValue + 1 };
        if (pl_vulkan_hold_ex(gpu, &holdParams)) {
            heldValue = ++m_State->renderedValue;
        }
        else if (!m_LoggedHoldFailure) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: pl_vulkan_hold_ex() failed for plane %d",
                         i);
            m_LoggedHoldFailure = true;
        }
    }

    // Submit the signals now, rather than with whatever the renderer does next
    pl_gpu_flush(gpu);

    if (heldValue != 0) {
        slot->renderedValue = heldValue;
    }
}

uint64_t PyrowaveVkDecoder::captureDecodeBoundary(AVFrame* frame)
{
    return ((PyrowaveVkSlot*)frame->data[0])->decodedValue;
}

// Waits on the CPU for PyroWave to finish decoding a frame, so the pacer schedules it
// from when it can actually be drawn
bool PyrowaveVkDecoder::waitForDecode(AVFrame* frame)
{
    uint64_t value = (uint64_t)ML_FRAME_DECODE_BOUNDARY(frame);
    if (!m_State->decodeWaitEnabled || value == 0) {
        return false;
    }

    VkDevice device = m_State->vulkan->device;

    uint64_t current = 0;
    if (m_State->getSemaphoreCounterValue(device, m_State->decodedSemaphore, &current) == VK_SUCCESS && current >= value) {
        return false;
    }

    VkSemaphoreWaitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &m_State->decodedSemaphore;
    waitInfo.pValues = &value;

    VkResult result = m_State->waitSemaphores(device, &waitInfo, 500ull * 1000 * 1000);
    if (result != VK_SUCCESS) {
        // Never risk stalling every frame on a semaphore that has stopped working
        m_State->decodeWaitEnabled = false;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "PyroWave: waiting for a frame to decode failed (%d); frames are presented without waiting from now on",
                    (int)result);
    }

    return true;
}
