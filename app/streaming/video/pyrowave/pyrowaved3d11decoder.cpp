#include "pyrowaved3d11decoder.h"
#include "pyrowaveapi.h"
#include "pyrowavestagetimer.h"

#include "streaming/video/ffmpeg-renderers/d3d11va.h"
#include "streaming/video/ffmpeg-renderers/pacer/pacer.h"

#include <QtGlobal>

#include <cstring>
#include <vector>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

using Microsoft::WRL::ComPtr;

// The pacer can hold on to all of its outstanding frames while we decode another
#define PYROWAVE_D3D11_PLANE_SETS (PACER_MAX_OUTSTANDING_FRAMES + 2)

// The PyroWave objects for decoding into one set of planes
struct PyrowaveD3D11SetHandles
{
    std::array<pyrowave_image, 3> planes = {};
    pyrowave_gpu_buffers buffers = {};
};

// The PyroWave objects for decoding into the D3D11 planes
struct PyrowaveD3D11Handles
{
    pyrowave_sync_object sync = nullptr;
    std::array<PyrowaveD3D11SetHandles, PYROWAVE_D3D11_PLANE_SETS> sets;
};

// How PyroWave uses the planes it imports from D3D11
static const VkImageUsageFlags k_PlaneImageUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

// Logs what the Vulkan driver reports for the interop with D3D11 that PyroWave needs:
// importing a D3D11 fence as a timeline semaphore, and importing D3D11 textures.
// PyroWave's own errors don't say which part the driver lacks.
static void logVulkanInteropSupport(const PyrowaveApi* api, pyrowave_device device, VkFormat planeFormat)
{
    if (api->deviceGetVkDeviceHandles == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "PyroWave: this library version can't report the Vulkan device's interop support");
        return;
    }

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    api->deviceGetVkDeviceHandles(device, &instance, &physicalDevice, nullptr);

    // PyroWave has already loaded the Vulkan loader
    HMODULE vulkan = GetModuleHandleW(L"vulkan-1.dll");
    auto getInstanceProcAddr = vulkan != nullptr ?
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vulkan, "vkGetInstanceProcAddr")) : nullptr;
    if (instance == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE || getInstanceProcAddr == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "PyroWave: couldn't get the Vulkan device to report its interop support");
        return;
    }

    auto getProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                getInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));
    auto getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
                getInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2"));
    auto enumerateExtensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
                getInstanceProcAddr(instance, "vkEnumerateDeviceExtensionProperties"));
    auto getSemaphoreProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>(
                getInstanceProcAddr(instance, "vkGetPhysicalDeviceExternalSemaphoreProperties"));
    auto getImageFormatProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
                getInstanceProcAddr(instance, "vkGetPhysicalDeviceImageFormatProperties2"));
    if (getProperties2 == nullptr || getFeatures2 == nullptr || enumerateExtensions == nullptr ||
            getSemaphoreProperties == nullptr || getImageFormatProperties2 == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "PyroWave: the Vulkan loader is missing entry points to report interop support");
        return;
    }

    VkPhysicalDeviceDriverProperties driverProps = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    props.pNext = &driverProps;
    getProperties2(physicalDevice, &props);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PyroWave: Vulkan device %s (Vulkan %u.%u.%u), driver %s %s",
                props.properties.deviceName,
                VK_API_VERSION_MAJOR(props.properties.apiVersion),
                VK_API_VERSION_MINOR(props.properties.apiVersion),
                VK_API_VERSION_PATCH(props.properties.apiVersion),
                driverProps.driverName,
                driverProps.driverInfo);

    VkPhysicalDeviceVulkan12Features vk12Features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceFeatures2 features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    features.pNext = &vk12Features;
    getFeatures2(physicalDevice, &features);

    uint32_t extensionCount = 0;
    enumerateExtensions(physicalDevice, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extensionCount);
    enumerateExtensions(physicalDevice, nullptr, &extensionCount, extensions.data());
    auto hasExtension = [&](const char* name) {
        for (const VkExtensionProperties& extension : extensions) {
            if (strcmp(extension.extensionName, name) == 0) {
                return true;
            }
        }
        return false;
    };

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PyroWave: timeline semaphores: %s, VK_KHR_external_semaphore_win32: %s, VK_KHR_external_memory_win32: %s",
                vk12Features.timelineSemaphore ? "yes" : "no",
                hasExtension("VK_KHR_external_semaphore_win32") ? "yes" : "no",
                hasExtension("VK_KHR_external_memory_win32") ? "yes" : "no");

    // PyroWave imports the D3D11 fence as a D3D12 fence (the same handle type). It checks
    // that handle type without asking for a timeline, as AMD reports no timeline support
    // for it, so the binary row is the one that decides.
    const struct {
        VkExternalSemaphoreHandleTypeFlagBits type;
        const char* name;
    } semaphoreHandleTypes[] = {
        { VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT, "D3D11/D3D12 fence" },
        { VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT, "opaque Win32" },
        { VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT, "opaque Win32 KMT" },
    };

    for (const auto& handleType : semaphoreHandleTypes) {
        for (VkSemaphoreType semaphoreType : { VK_SEMAPHORE_TYPE_BINARY, VK_SEMAPHORE_TYPE_TIMELINE }) {
            VkSemaphoreTypeCreateInfo typeInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
            typeInfo.semaphoreType = semaphoreType;

            VkPhysicalDeviceExternalSemaphoreInfo semaphoreInfo = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO };
            semaphoreInfo.pNext = &typeInfo;
            semaphoreInfo.handleType = handleType.type;

            VkExternalSemaphoreProperties semaphoreProps = { VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES };
            getSemaphoreProperties(physicalDevice, &semaphoreInfo, &semaphoreProps);

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "PyroWave: %s %s semaphores: import %s, export %s",
                        handleType.name,
                        semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE ? "timeline" : "binary",
                        (semaphoreProps.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) ? "yes" : "no",
                        (semaphoreProps.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) ? "yes" : "no");
        }
    }

    // The planes are imported as D3D11 textures with an NT handle. See createPlanes().
    const struct {
        VkExternalMemoryHandleTypeFlagBits type;
        const char* name;
    } memoryHandleTypes[] = {
        { VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, "D3D11 texture" },
        { VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT, "D3D11 texture KMT" },
    };

    for (const auto& handleType : memoryHandleTypes) {
        VkPhysicalDeviceExternalImageFormatInfo externalInfo = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO };
        externalInfo.handleType = handleType.type;

        VkPhysicalDeviceImageFormatInfo2 formatInfo = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2 };
        formatInfo.pNext = &externalInfo;
        formatInfo.format = planeFormat;
        formatInfo.type = VK_IMAGE_TYPE_2D;
        formatInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        formatInfo.usage = k_PlaneImageUsage;

        VkExternalImageFormatProperties externalProps = { VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
        VkImageFormatProperties2 formatProps = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };
        formatProps.pNext = &externalProps;
        VkResult vkResult = getImageFormatProperties2(physicalDevice, &formatInfo, &formatProps);

        VkExternalMemoryFeatureFlags memoryFeatures = externalProps.externalMemoryProperties.externalMemoryFeatures;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "PyroWave: %s %s images: %s (%d), import %s, dedicated only %s",
                    handleType.name,
                    planeFormat == VK_FORMAT_R16_UNORM ? "R16" : "R8",
                    vkResult == VK_SUCCESS ? "supported" : "unsupported",
                    vkResult,
                    (memoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ? "yes" : "no",
                    (memoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) ? "yes" : "no");
    }
}

PyrowaveD3D11Decoder::PyrowaveD3D11Decoder()
    : m_D3D11Handles(new PyrowaveD3D11Handles()),
      m_Lock(nullptr),
      m_Unlock(nullptr),
      m_LockContext(nullptr),
      m_LoggedNoFreeSet(false),
      m_FenceValue(0),
      m_CpuSync(false),
      m_ReleaseValue(0),
      m_ReleaseEvent(nullptr)
{
}

PyrowaveD3D11Decoder::~PyrowaveD3D11Decoder()
{
    // Waits for the GPU to be done with the planes
    destroyDecoder();

    if (m_Api != nullptr) {
        for (PyrowaveD3D11SetHandles& set : m_D3D11Handles->sets) {
            for (pyrowave_image plane : set.planes) {
                if (plane != nullptr) {
                    m_Api->imageDestroy(plane);
                }
            }
        }
        if (m_D3D11Handles->sync != nullptr) {
            m_Api->syncObjectDestroy(m_D3D11Handles->sync);
        }
    }

    delete m_D3D11Handles;

    if (m_ReleaseEvent != nullptr) {
        CloseHandle(m_ReleaseEvent);
    }
}

void PyrowaveD3D11Decoder::lockContext()
{
    m_Lock(m_LockContext);
}

void PyrowaveD3D11Decoder::unlockContext()
{
    m_Unlock(m_LockContext);
}

bool PyrowaveD3D11Decoder::initialize(D3D11VARenderer* renderer, PDECODER_PARAMETERS params, int colorspace)
{
    HRESULT hr;

    if (!initializeStream(params, colorspace)) {
        return false;
    }

    // The planes go on the renderer's decode device, like a hardware decoder's frames
    {
        AVD3D11VADeviceContext* d3d11vaDeviceContext = renderer->getDecodeDeviceContext();

        hr = d3d11vaDeviceContext->device->QueryInterface(IID_PPV_ARGS(&m_Device));
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: ID3D11Device::QueryInterface(ID3D11Device5) failed: %x",
                         hr);
            return false;
        }

        hr = d3d11vaDeviceContext->device_context->QueryInterface(IID_PPV_ARGS(&m_DeviceContext));
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: ID3D11DeviceContext::QueryInterface(ID3D11DeviceContext4) failed: %x",
                         hr);
            return false;
        }

        m_Lock = d3d11vaDeviceContext->lock;
        m_Unlock = d3d11vaDeviceContext->unlock;
        m_LockContext = d3d11vaDeviceContext->lock_ctx;
    }

    // Decode on the same GPU in Vulkan
    DXGI_ADAPTER_DESC adapterDesc;
    {
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        hr = m_Device.As(&dxgiDevice);
        if (SUCCEEDED(hr)) {
            hr = dxgiDevice->GetAdapter(&adapter);
        }
        if (SUCCEEDED(hr)) {
            hr = adapter->GetDesc(&adapterDesc);
        }
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: couldn't find the D3D11 device's adapter: %x",
                         hr);
            return false;
        }
    }

    static_assert(sizeof(LUID) == sizeof(pyrowave_luid), "LUID size mismatch");
    pyrowave_luid luid;
    memcpy(luid.luid, &adapterDesc.AdapterLuid, sizeof(luid.luid));

    // High priority puts PyroWave on an async compute queue. Without the privilege for
    // it, the driver gives a lower one.
    pyrowave_result result = m_Api->createDeviceByCompat2(0, 0, nullptr, nullptr, &luid,
                                                          VK_QUEUE_GLOBAL_PRIORITY_HIGH,
                                                          &m_Handles->device);
    if (result != PYROWAVE_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: couldn't create a Vulkan device for %S (%d). A Vulkan 1.3 driver is required.",
                     adapterDesc.Description,
                     result);
        return false;
    }

    if (!createSync() || !createPlaneSets() || !createDecoder()) {
        return false;
    }

    std::vector<std::array<ID3D11Texture2D*, 3>> planeSets;
    for (const PlaneSet& set : m_PlaneSets) {
        planeSets.push_back({ set.textures[0].Get(), set.textures[1].Get(), set.textures[2].Get() });
    }
    if (!renderer->setPlaneSets(planeSets)) {
        return false;
    }

    // Nothing follows the decode before the renderer draws the planes
    m_StageTimer = PyrowaveStageTimer::createIfEnabled(m_Api->syncObjectCpuWait);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PyroWave: decoding %dx%d %s %s on %S",
                m_Width, m_Height,
                m_TenBit ? "10-bit" : "8-bit",
                m_Yuv444 ? "4:4:4" : "4:2:0",
                adapterDesc.Description);
    return true;
}

// Orders D3D11's drawing of a frame after PyroWave's decode of it. The best way is a
// D3D11 fence PyroWave imports. Some drivers, like Intel's last one for Gen9, can't
// import one but can export a Vulkan timeline semaphore, which D3D11 may be able to open
// as a fence. Failing both, the decoder thread waits for each decode on the CPU.
// ML_PYROWAVE_CPU_SYNC=1 forces that.
//
// Also creates the fence that says when the renderer is done with a set of planes.
bool PyrowaveD3D11Decoder::createSync()
{
    HRESULT hr;
    pyrowave_result result;

    hr = m_Device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_ReleaseFence));
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: ID3D11Device5::CreateFence() failed: %x",
                     hr);
        return false;
    }

    m_ReleaseEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (m_ReleaseEvent == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: CreateEvent() failed: %d",
                     (int)GetLastError());
        return false;
    }

    bool forceCpuSync = qEnvironmentVariableIntValue("ML_PYROWAVE_CPU_SYNC") != 0;

    pyrowave_sync_object_create_info syncInfo = {};
    syncInfo.device = m_Handles->device;
    syncInfo.semaphore_type = VK_SEMAPHORE_TYPE_TIMELINE;

    if (!forceCpuSync) {
        hr = m_Device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_Fence));
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: ID3D11Device5::CreateFence() failed: %x",
                         hr);
            return false;
        }

        HANDLE fenceHandle;
        hr = m_Fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fenceHandle);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: ID3D11Fence::CreateSharedHandle() failed: %x",
                         hr);
            return false;
        }

        syncInfo.external_handle = (pyrowave_os_handle)fenceHandle;
        syncInfo.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
        result = m_Api->syncObjectCreate(&syncInfo, &m_D3D11Handles->sync);
        if (result == PYROWAVE_SUCCESS) {
            return true;
        }

        CloseHandle(fenceHandle);
        m_Fence.Reset();

        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "PyroWave: couldn't import a D3D11 fence (%d)",
                    result);
        logVulkanInteropSupport(m_Api, m_Handles->device, m_TenBit ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM);
    }

    // A timeline semaphore to export. PyroWave requires the TEMPORARY flag to create
    // one without a handle to import, though it's otherwise unused.
    syncInfo.external_handle = 0;
    syncInfo.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    syncInfo.import_flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
    result = m_Api->syncObjectCreate(&syncInfo, &m_D3D11Handles->sync);
    if (result != PYROWAVE_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: couldn't create a timeline semaphore (%d)",
                     result);
        return false;
    }

    if (!forceCpuSync) {
        pyrowave_os_handle semaphoreHandle;
        result = m_Api->syncObjectExportHandle(m_D3D11Handles->sync, &semaphoreHandle);
        if (result == PYROWAVE_SUCCESS) {
            hr = m_Device->OpenSharedFence((HANDLE)semaphoreHandle, IID_PPV_ARGS(&m_Fence));
            CloseHandle((HANDLE)semaphoreHandle);
            if (FAILED(hr)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "PyroWave: ID3D11Device5::OpenSharedFence() failed: %x",
                            hr);
            }
            else if (testSharedFence()) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "PyroWave: synchronizing with D3D11 through a fence exported from Vulkan");
                return true;
            }
            else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "PyroWave: D3D11 opened the fence exported from Vulkan, but they don't see each other's signals");
                m_Fence.Reset();
            }
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "PyroWave: couldn't export the timeline semaphore (%d)",
                        result);
        }
    }

    m_CpuSync = true;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "PyroWave: synchronizing with D3D11 on the CPU, which adds a little latency");
    return true;
}

// Whether a signal on either side of the fence D3D11 opened from PyroWave's timeline
// semaphore reaches the other. Some drivers (Intel's for Gen9) open it without error, but
// then neither sees the other's signals and decoding stalls forever. Uses values 1 and 2.
bool PyrowaveD3D11Decoder::testSharedFence()
{
    // The semaphore's value is at least 2 afterwards, whether or not this works, so the
    // values decodePushedFrame() signals have to start above it
    m_FenceValue = 2;

    // Vulkan to D3D11
    if (m_Api->syncObjectCpuSignal(m_D3D11Handles->sync, 1) != PYROWAVE_SUCCESS) {
        return false;
    }

    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr) {
        return false;
    }

    bool seen = SUCCEEDED(m_Fence->SetEventOnCompletion(1, event)) &&
                WaitForSingleObject(event, 100) == WAIT_OBJECT_0;
    CloseHandle(event);
    if (!seen) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "PyroWave: D3D11 didn't see a signal from Vulkan");
        return false;
    }

    // D3D11 to Vulkan
    lockContext();
    m_DeviceContext->Signal(m_Fence.Get(), 2);
    m_DeviceContext->Flush();
    unlockContext();

    if (m_Api->syncObjectCpuWait(m_D3D11Handles->sync, 2, 100000000ULL) != PYROWAVE_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "PyroWave: Vulkan didn't see a signal from D3D11");
        return false;
    }

    return true;
}

bool PyrowaveD3D11Decoder::createPlaneSets()
{
    m_PlaneSets.resize(PYROWAVE_D3D11_PLANE_SETS);

    for (int set = 0; set < PYROWAVE_D3D11_PLANE_SETS; set++) {
        m_PlaneSets[set].index = set;
        m_PlaneSets[set].releasedValue = 0;

        for (int i = 0; i < 3; i++) {
            HRESULT hr;

            // Cb and Cr are half size for 4:2:0
            bool fullSize = i == 0 || m_Yuv444;
            int planeWidth = fullSize ? m_Width : m_Width / 2;
            int planeHeight = fullSize ? m_Height : m_Height / 2;

            D3D11_TEXTURE2D_DESC texDesc = {};
            texDesc.Width = planeWidth;
            texDesc.Height = planeHeight;
            texDesc.MipLevels = 1;
            texDesc.ArraySize = 1;
            texDesc.Format = m_TenBit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
            texDesc.SampleDesc.Count = 1;
            texDesc.Usage = D3D11_USAGE_DEFAULT;
            // PyroWave writes these as storage images
            texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

            hr = m_Device->CreateTexture2D(&texDesc, nullptr, &m_PlaneSets[set].textures[i]);
            if (FAILED(hr)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "PyroWave: ID3D11Device::CreateTexture2D() failed: %x",
                             hr);
                return false;
            }

            ComPtr<IDXGIResource1> resource;
            HANDLE textureHandle;
            hr = m_PlaneSets[set].textures[i].As(&resource);
            if (SUCCEEDED(hr)) {
                hr = resource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &textureHandle);
            }
            if (FAILED(hr)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "PyroWave: IDXGIResource1::CreateSharedHandle() failed: %x",
                             hr);
                return false;
            }

            // Must match the D3D11 texture closely enough for the driver to import it
            VkImageCreateInfo imageCreateInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
            imageCreateInfo.format = m_TenBit ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;
            imageCreateInfo.extent = { (uint32_t)planeWidth, (uint32_t)planeHeight, 1 };
            imageCreateInfo.mipLevels = 1;
            imageCreateInfo.arrayLayers = 1;
            imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageCreateInfo.usage = k_PlaneImageUsage;
            imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            PyrowaveD3D11SetHandles& setHandles = m_D3D11Handles->sets[set];

            pyrowave_image_create_info imageInfo = {};
            imageInfo.device = m_Handles->device;
            imageInfo.external_handle = (pyrowave_os_handle)textureHandle;
            imageInfo.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
            imageInfo.image_create_info = &imageCreateInfo;
            pyrowave_result result = m_Api->imageCreate(&imageInfo, &setHandles.planes[i]);
            if (result != PYROWAVE_SUCCESS) {
                // Whether PyroWave closed the handle on failure isn't specified, so it is left open
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "PyroWave: couldn't import plane %d (%d)",
                             i, result);
                return false;
            }

            result = m_Api->imageGetImageView(setHandles.planes[i],
                                              VK_IMAGE_ASPECT_COLOR_BIT,
                                              VK_IMAGE_USAGE_STORAGE_BIT,
                                              &setHandles.buffers.planes[i]);
            if (result != PYROWAVE_SUCCESS) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "PyroWave: couldn't view plane %d (%d)",
                             i, result);
                return false;
            }
        }

        m_FreeSets.push_back(set);
    }

    return true;
}

void PyrowaveD3D11Decoder::flush()
{
    lockContext();
    m_DeviceContext->Flush();
    unlockContext();
}

AVFrame* PyrowaveD3D11Decoder::decodePushedFrame()
{
    pyrowave_result result;

    PlaneSet* set;
    {
        std::lock_guard<std::mutex> lock(m_FreeSetsLock);
        if (m_FreeSets.empty()) {
            set = nullptr;
        }
        else {
            set = &m_PlaneSets[m_FreeSets.back()];
            m_FreeSets.pop_back();
        }
    }

    // Every set is held by a frame waiting to be rendered
    if (set == nullptr) {
        if (!m_LoggedNoFreeSet) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "PyroWave: no free frame to decode into");
            m_LoggedNoFreeSet = true;
        }
        return nullptr;
    }

    auto returnSet = [this, set]() {
        std::lock_guard<std::mutex> lock(m_FreeSetsLock);
        m_FreeSets.push_back(set->index);
    };

    // Wait for the renderer to be done with the planes. It normally has long since, as
    // a set is released only once the frame after it has been drawn.
    if (set->releasedValue != 0 && m_ReleaseFence->GetCompletedValue() < set->releasedValue) {
        HRESULT hr = m_ReleaseFence->SetEventOnCompletion(set->releasedValue, m_ReleaseEvent);
        if (FAILED(hr) || WaitForSingleObject(m_ReleaseEvent, 1000) != WAIT_OBJECT_0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: timed out waiting for the renderer to finish with a frame");
            returnSet();
            return nullptr;
        }
    }

    PyrowaveD3D11SetHandles& setHandles = m_D3D11Handles->sets[set->index];

    // The planes' old contents can be discarded. They go to D3D11 when decoded.
    std::array<pyrowave_gpu_external_reference, 3> acquireRefs, releaseRefs;
    for (int i = 0; i < 3; i++) {
        acquireRefs[i] = { setHandles.planes[i], VK_QUEUE_FAMILY_IGNORED };
        releaseRefs[i] = { setHandles.planes[i], VK_QUEUE_FAMILY_EXTERNAL };
    }

    uint64_t decodedValue = ++m_FenceValue;

    pyrowave_gpu_sync_operation acquire = {};
    acquire.images = acquireRefs.data();
    acquire.num_images = acquireRefs.size();

    pyrowave_gpu_sync_operation release = {};
    release.images = releaseRefs.data();
    release.num_images = releaseRefs.size();
    release.sync = { m_Api->syncObjectGetSemaphore(m_D3D11Handles->sync), decodedValue };

    result = m_Api->decoderDecodeGpuBuffer(m_Handles->decoder, &acquire, &release, &setHandles.buffers);
    if (result != PYROWAVE_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: decoding failed (%d)",
                     result);
        returnSet();
        return nullptr;
    }

    if (m_StageTimer != nullptr) {
        m_StageTimer->decodeSubmitted(m_D3D11Handles->sync, decodedValue);
    }

    if (m_CpuSync) {
        // Wait here for PyroWave to finish decoding before D3D11 draws the planes
        result = m_Api->syncObjectCpuWait(m_D3D11Handles->sync, decodedValue, 1000000000ULL);
        if (result != PYROWAVE_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: waiting for the decode failed (%d)",
                         result);
            returnSet();
            return nullptr;
        }
    }
    else {
        // Or have the decode device's context wait for it on the GPU. The renderer's
        // drawing follows that context: it is the render context too, or the renderer
        // waits on the fence the decoder thread signals on it next (see
        // D3D11VARenderer::captureDecodeBoundary()).
        lockContext();
        m_DeviceContext->Wait(m_Fence.Get(), decodedValue);
        m_DeviceContext->Flush();
        unlockContext();
    }

    AVFrame* frame = av_frame_alloc();
    if (frame == nullptr) {
        returnSet();
        return nullptr;
    }

    // The frame holds the set until it's freed. Its format says how the planes are laid
    // out (PyroWave fills the whole range of the 16-bit planes for 10-bit), but its data
    // is the set and its index, never pixels. Only the renderer reads it.
    frame->buf[0] = av_buffer_create((uint8_t*)set, sizeof(*set), releaseSet, this, 0);
    if (frame->buf[0] == nullptr) {
        av_frame_free(&frame);
        returnSet();
        return nullptr;
    }
    frame->data[0] = (uint8_t*)set;
    frame->data[1] = (uint8_t*)(uintptr_t)set->index;
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

// Frees a frame's set for decoding into again, when the last reference to the frame
// goes. That is after the renderer has drawn the frame after it, so the decode device's
// context is already ordered after the drawing from these planes: it is the render
// context, or it waits for the render context's previous frame (see
// D3D11VARenderer::renderVideo()). A signal on it now passes once that drawing is done.
void PyrowaveD3D11Decoder::releaseSet(void* opaque, uint8_t* data)
{
    auto me = (PyrowaveD3D11Decoder*)opaque;
    auto set = (PlaneSet*)data;

    me->lockContext();
    uint64_t value = ++me->m_ReleaseValue;
    if (SUCCEEDED(me->m_DeviceContext->Signal(me->m_ReleaseFence.Get(), value))) {
        set->releasedValue = value;
    }
    me->m_DeviceContext->Flush();
    me->unlockContext();

    std::lock_guard<std::mutex> lock(me->m_FreeSetsLock);
    me->m_FreeSets.push_back(set->index);
}
