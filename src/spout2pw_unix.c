#if 0 // NOTE: Never dare forgot that, otherwise makedep will not put the file
      // to the unix lib
#pragma makedep unix
#endif

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include <funnel-vk.h>
#include <funnel.h>

#include "spout2pw_unix.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(spout2pw);

#define API_VERSION VK_API_VERSION_1_0
// #define HAVE_VK_1_1
// #define HAVE_VK_1_2

#define D3DFMT_A8R8G8B8 21
#define D3DFMT_X8R8G8B8 22

#define DXGI_FORMAT_R32G32B32A32_FLOAT 2
#define DXGI_FORMAT_R16G16B16A16_FLOAT 10
#define DXGI_FORMAT_R16G16B16A16_UNORM 11
#define DXGI_FORMAT_R16G16B16A16_SNORM 13
#define DXGI_FORMAT_R10G10B10A2_UNORM 24
#define DXGI_FORMAT_R8G8B8A8_UNORM 28
#define DXGI_FORMAT_R8G8B8A8_UNORM_SRGB 29
#define DXGI_FORMAT_R8G8B8A8_SNORM 31
#define DXGI_FORMAT_B8G8R8A8_UNORM 87
#define DXGI_FORMAT_B8G8R8X8_UNORM 88

#define D3D11_BIND_SHADER_RESOURCE 0x8
#define D3D11_BIND_RENDER_TARGET 0x20
#define D3D11_BIND_UNORDERED_ACCESS 0x80

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_core.h>

#define CHECK_VK_RESULT(_expr)                                                 \
    result = _expr;                                                            \
    if (result != VK_SUCCESS) {                                                \
        ERR("Vulkan error on %s: %i\n", #_expr, result);                       \
    }                                                                          \
    if (result != VK_SUCCESS)

#define GET_EXTENSION_FUNCTION(_id)                                            \
    ((PFN_##_id)(vkGetInstanceProcAddr(instance, #_id)))

static struct startup_params startup_params = {0};
struct funnel_ctx *funnel;

struct source {
    void *receiver;
    struct funnel_stream *stream;
    VkCommandBuffer commandBuffer;
    VkDeviceMemory mem;
    VkImage image;

    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool quit;
    struct source_info info;
    int cur_fd;
    uint32_t width;
    uint32_t height;
    bool update;
    bool dead;
};

static NTSTATUS errno_to_status(int err) {
    WINE_TRACE("errno = %d\n", err);
    switch (err) {
    case EINVAL:
        return STATUS_INVALID_PARAMETER;

    case ENOMEDIUM:
        return STATUS_NO_MEDIA;

    case ENOMEM:
        return STATUS_NO_MEMORY;

    case ESOCKTNOSUPPORT:
        return STATUS_PROTOCOL_UNREACHABLE;

    case ENOTCONN:
        return STATUS_CONNECTION_INVALID;

    case EPERM:
        return STATUS_ACCESS_DENIED;

    case EOPNOTSUPP:
        return STATUS_NOT_SUPPORTED;

    case ENXIO:
        return STATUS_NO_SUCH_DEVICE;

    case EBADMSG:
        return STATUS_INVALID_MESSAGE;

    case EBUSY:
        return STATUS_DEVICE_BUSY;

    case EMFILE:
        return STATUS_TOO_MANY_OPENED_FILES;

    case ESTALE:
        return STATUS_ALREADY_DISCONNECTED;

    default:
        WINE_FIXME("Converting errno %d to STATUS_UNSUCCESSFUL\n", err);
        return STATUS_UNSUCCESSFUL;
    }
}

static struct lock_texture_return *lock_texture(void *receiver) {
    void *ret_ptr;
    ULONG ret_len;
    struct receiver_params params = {
        .dispatch = {.callback = startup_params.lock_texture},
        .receiver = receiver,
    };
    TRACE("params=%p/%p receiver=%p\n", &params, &params.dispatch, receiver);

    if (KeUserDispatchCallback(&params.dispatch, sizeof(params), &ret_ptr,
                               &ret_len))
        return NULL;
    if (ret_ptr && ret_len == sizeof(struct lock_texture_return))
        return (struct lock_texture_return *)ret_ptr;
    else
        return NULL;
}

static void unlock_texture(void *receiver) {
    void *ret_ptr;
    ULONG ret_len;
    struct receiver_params params = {
        .dispatch = {.callback = startup_params.unlock_texture},
        .receiver = receiver,
    };
    if (KeUserDispatchCallback(&params.dispatch, sizeof(params), &ret_ptr,
                               &ret_len))
        return;
}

static const char *const appName = "Spout2Pw";
static const char *const instanceExtensionNames[] = {
    "VK_EXT_debug_utils",

#ifndef HAVE_VK_1_1
    "VK_KHR_get_physical_device_properties2",
    "VK_KHR_external_memory_capabilities",
    "VK_KHR_external_semaphore_capabilities",
#endif
};

static const char *const deviceExtensionNames[] = {

#ifndef HAVE_VK_1_1
    "VK_KHR_external_memory",
    "VK_KHR_maintenance1",
    "VK_KHR_bind_memory2",
    "VK_KHR_sampler_ycbcr_conversion",
    "VK_KHR_get_memory_requirements2",
    "VK_KHR_external_semaphore",
#endif
#ifndef HAVE_VK_1_2
    "VK_KHR_image_format_list",
#endif

    "VK_KHR_external_semaphore_fd",
    "VK_KHR_external_memory_fd",
    "VK_EXT_external_memory_dma_buf",
    "VK_EXT_image_drm_format_modifier",
};
static const char *const layerNames[] = {"VK_LAYER_KHRONOS_validation"};
static VkInstance instance = VK_NULL_HANDLE;
static VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
static VkPhysicalDevice physDevice = VK_NULL_HANDLE;
static VkDevice device = VK_NULL_HANDLE;
static uint32_t queueFamilyIndex = 0;
static VkQueue queue = VK_NULL_HANDLE;
static VkCommandPool commandPool = VK_NULL_HANDLE;
static uint32_t preferredMemoryTypeBits;

struct {
    PFN_vkGetMemoryFdPropertiesKHR vkGetMemoryFdPropertiesKHR;
    PFN_vkGetImageMemoryRequirements2KHR vkGetImageMemoryRequirements2KHR;

} vk;

static VkBool32
onError(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT type,
        const VkDebugUtilsMessengerCallbackDataEXT *callbackData,
        void *userData) {
    ERR("Vulkan ");

    switch (type) {
    case VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT:
        ERR("general ");
        break;
    case VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT:
        ERR("validation ");
        break;
    case VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT:
        ERR("performance ");
        break;
    }

    switch (severity) {
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
        ERR("(verbose): ");
        break;
    default:
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
        ERR("(info): ");
        break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
        ERR("(warning): ");
        break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
        ERR("(error): ");
        break;
    }

    ERR("%s\n", callbackData->pMessage);

    return 0;
}

static bool getflag(const char *name) {
    const char *val = getenv("SPOUT2PW_VALIDATION");

    if (!val)
        return false;

    return !strcmp(val, "1");
}

static NTSTATUS startup(void *args) {
    struct startup_params *params = args;

    startup_params = *params;

    VkResult result;

    {
        VkApplicationInfo appInfo = {0};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = appName;
        appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.pEngineName = appName;
        appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.apiVersion = API_VERSION;

        VkInstanceCreateInfo createInfo = {0};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount =
            sizeof(instanceExtensionNames) / sizeof(const char *);
        createInfo.ppEnabledExtensionNames = instanceExtensionNames;

        size_t foundLayers = 0;

        uint32_t deviceLayerCount;
        CHECK_VK_RESULT(
            vkEnumerateInstanceLayerProperties(&deviceLayerCount, NULL)) {
            return STATUS_FATAL_APP_EXIT;
        }

        VkLayerProperties *layerProperties =
            malloc(deviceLayerCount * sizeof(VkLayerProperties));
        CHECK_VK_RESULT(vkEnumerateInstanceLayerProperties(&deviceLayerCount,
                                                           layerProperties)) {
            return STATUS_FATAL_APP_EXIT;
        }

        for (uint32_t i = 0; i < deviceLayerCount; i++) {
            for (size_t j = 0; j < sizeof(layerNames) / sizeof(const char *);
                 j++) {
                if (strcmp(layerProperties[i].layerName, layerNames[j]) == 0) {
                    foundLayers++;
                }
            }
        }

        free(layerProperties);

        if (getflag("SPOUT2PW_VALIDATION")) {
            if (foundLayers >= sizeof(layerNames) / sizeof(const char *)) {
                createInfo.enabledLayerCount =
                    sizeof(layerNames) / sizeof(const char *);
                createInfo.ppEnabledLayerNames = layerNames;
            }
        }

        CHECK_VK_RESULT(vkCreateInstance(&createInfo, NULL, &instance)) {
            return STATUS_FATAL_APP_EXIT;
        }
    }

    {
        VkDebugUtilsMessengerCreateInfoEXT createInfo = {0};
        createInfo.sType =
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        createInfo.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        createInfo.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        createInfo.pfnUserCallback = onError;

        CHECK_VK_RESULT(GET_EXTENSION_FUNCTION(vkCreateDebugUtilsMessengerEXT)(
            instance, &createInfo, NULL, &debugMessenger)) {
            return STATUS_FATAL_APP_EXIT;
        }
    }

    uint32_t physDeviceCount;
    vkEnumeratePhysicalDevices(instance, &physDeviceCount, NULL);

    VkPhysicalDevice physDevices[physDeviceCount];
    vkEnumeratePhysicalDevices(instance, &physDeviceCount, physDevices);

    uint32_t bestScore = 0;

    for (uint32_t i = 0; i < physDeviceCount; i++) {
        VkPhysicalDevice device = physDevices[i];

        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(device, &properties);

        uint32_t score;

        switch (properties.deviceType) {
        default:
            continue;
        case VK_PHYSICAL_DEVICE_TYPE_OTHER:
            score = 1;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            score = 4;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            score = 5;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            score = 3;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            score = 2;
            break;
        }

        if (score > bestScore) {
            physDevice = device;
            bestScore = score;
        }
    }

    {
        uint32_t queueFamilyCount;
        vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &queueFamilyCount,
                                                 NULL);

        VkQueueFamilyProperties queueFamilies[queueFamilyCount];
        vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &queueFamilyCount,
                                                 queueFamilies);

        for (uint32_t i = 0; i < queueFamilyCount; i++) {
            if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                queueFamilyIndex = i;
                break;
            }
        }

        float priority = 1;

        VkDeviceQueueCreateInfo queueCreateInfo = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = queueFamilyIndex,
            .queueCount = 1,
            .pQueuePriorities = &priority,
        };

        VkDeviceCreateInfo createInfo = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queueCreateInfo,
            .enabledExtensionCount =
                sizeof(deviceExtensionNames) / sizeof(const char *),
            .ppEnabledExtensionNames = deviceExtensionNames,
        };

        uint32_t deviceLayerCount;
        CHECK_VK_RESULT(vkEnumerateDeviceLayerProperties(
            physDevice, &deviceLayerCount, NULL)) {
            return STATUS_FATAL_APP_EXIT;
        }

        VkLayerProperties *layerProperties =
            malloc(deviceLayerCount * sizeof(VkLayerProperties));
        CHECK_VK_RESULT(vkEnumerateDeviceLayerProperties(
            physDevice, &deviceLayerCount, layerProperties)) {
            return STATUS_FATAL_APP_EXIT;
        }

        size_t foundLayers = 0;

        for (uint32_t i = 0; i < deviceLayerCount; i++) {
            for (size_t j = 0; j < sizeof(layerNames) / sizeof(const char *);
                 j++) {
                if (strcmp(layerProperties[i].layerName, layerNames[j]) == 0) {
                    foundLayers++;
                }
            }
        }

        free(layerProperties);

        if (foundLayers >= sizeof(layerNames) / sizeof(const char *)) {
            createInfo.enabledLayerCount =
                sizeof(layerNames) / sizeof(const char *);
            createInfo.ppEnabledLayerNames = layerNames;
        }

        CHECK_VK_RESULT(
            vkCreateDevice(physDevice, &createInfo, NULL, &device)) {
            return STATUS_FATAL_APP_EXIT;
        }

        vk.vkGetImageMemoryRequirements2KHR =
            (PFN_vkGetImageMemoryRequirements2KHR)vkGetDeviceProcAddr(
                device, "vkGetImageMemoryRequirements2");

        if (!vk.vkGetImageMemoryRequirements2KHR)
            vk.vkGetImageMemoryRequirements2KHR =
                (PFN_vkGetImageMemoryRequirements2KHR)vkGetDeviceProcAddr(
                    device, "vkGetImageMemoryRequirements2KHR");

        vk.vkGetMemoryFdPropertiesKHR =
            (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(
                device, "vkGetMemoryFdPropertiesKHR");

        vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);
    }

    {
        VkCommandPoolCreateInfo createInfo = {0};
        createInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        createInfo.queueFamilyIndex = queueFamilyIndex;
        createInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        CHECK_VK_RESULT(
            vkCreateCommandPool(device, &createInfo, NULL, &commandPool)) {
            return STATUS_FATAL_APP_EXIT;
        }
    }

    {
        VkPhysicalDeviceMemoryProperties memoryProperties;

        vkGetPhysicalDeviceMemoryProperties(physDevice, &memoryProperties);

        preferredMemoryTypeBits = 0;
        for (int i = 0; i < memoryProperties.memoryTypeCount; i++) {
            if (memoryProperties.memoryTypes[i].propertyFlags &
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                preferredMemoryTypeBits |= 1L << i;
        }
    }

    int ret = funnel_init(&funnel);
    if (ret) {
        ERR("libfunnel initialization failed: %d\n", ret);
        return errno_to_status(-ret);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS create_source(void *args) {
    VkResult result;
    int ret = -EINVAL;

    struct create_source_params *params = args;
    struct source *source;
    struct funnel_stream *stream;

    source = calloc(1, sizeof(*source));

    VkCommandBufferAllocateInfo allocInfo = {0};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.commandBufferCount = 1;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;

    CHECK_VK_RESULT(
        vkAllocateCommandBuffers(device, &allocInfo, &source->commandBuffer)) {
        ret = -EIO;
        goto free_source;
    }

    ret = funnel_stream_create(funnel, params->sender_name, &stream);
    if (ret) {
        ERR("Failed to create libfunnel stream\n");
        goto free_cmdbufs;
    }

    ret = funnel_stream_init_vulkan(stream, instance, physDevice, device);
    if (ret) {
        ERR("Funnel Vulkan init failed\n");
        goto free_stream;
    }

    ret = funnel_stream_set_mode(stream, FUNNEL_SYNCHRONOUS);
    if (ret)
        goto free_stream;

    ret =
        funnel_stream_set_rate(stream, FUNNEL_RATE_VARIABLE,
                               FUNNEL_FRACTION(1, 1), FUNNEL_FRACTION(1000, 1));
    if (ret)
        goto free_stream;

    ret = funnel_stream_vk_set_usage(stream, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (ret)
        goto free_stream;

    bool have_format = false;
    ret = funnel_stream_vk_add_format(stream, VK_FORMAT_R8G8B8A8_SRGB, true,
                                      VK_FORMAT_FEATURE_BLIT_DST_BIT);
    have_format |= ret == 0;
    ret = funnel_stream_vk_add_format(stream, VK_FORMAT_B8G8R8A8_SRGB, true,
                                      VK_FORMAT_FEATURE_BLIT_DST_BIT);
    have_format |= ret == 0;
    ret = funnel_stream_vk_add_format(stream, VK_FORMAT_R8G8B8A8_SRGB, false,
                                      VK_FORMAT_FEATURE_BLIT_DST_BIT);
    have_format |= ret == 0;
    ret = funnel_stream_vk_add_format(stream, VK_FORMAT_B8G8R8A8_SRGB, false,
                                      VK_FORMAT_FEATURE_BLIT_DST_BIT);
    have_format |= ret == 0;

    if (!have_format) {
        ERR("No Vulkan formats compatible\n");
        ret = -EINVAL;
        goto free_stream;
    }

    pthread_mutex_init(&source->lock, NULL);
    pthread_cond_init(&source->cond, NULL);
    source->stream = stream;
    source->update = true; /// Initial update
    source->info = params->info;
    source->receiver = params->receiver;
    source->cur_fd = -1;
    params->ret_source = source;
    return STATUS_SUCCESS;

free_stream:
    funnel_stream_destroy(stream);
free_cmdbufs:
    vkFreeCommandBuffers(device, commandPool, 1, &source->commandBuffer);
free_source:
    free(source);
    return errno_to_status(-ret);
}

static void free_texture(struct source *source) {
    TRACE("Freeing texture\n");

    if (source->image != VK_NULL_HANDLE)
        vkDestroyImage(device, source->image, NULL);
    source->image = VK_NULL_HANDLE;
    if (source->mem != VK_NULL_HANDLE)
        vkFreeMemory(device, source->mem, NULL);
    source->mem = VK_NULL_HANDLE;

    if (source->cur_fd != -1) {
        close(source->cur_fd);
        source->cur_fd = -1;
    }

    TRACE("Texture freed\n");
}

struct format_alpha {
    VkFormat format;
    bool alpha;
};

static struct format_alpha dx_to_vkformat(uint32_t format) {
    TRACE("Format: %d\n", format);
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return (struct format_alpha){VK_FORMAT_R32G32B32A32_SFLOAT, true};
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return (struct format_alpha){VK_FORMAT_R16G16B16A16_SFLOAT, true};
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        return (struct format_alpha){VK_FORMAT_R16G16B16A16_UNORM, true};
    case DXGI_FORMAT_R16G16B16A16_SNORM:
        return (struct format_alpha){VK_FORMAT_R16G16B16A16_SNORM, true};
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return (struct format_alpha){VK_FORMAT_A2R10G10B10_UNORM_PACK32, true};

    // Note: Force SRGB, non-SRGB makes no sense.
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return (struct format_alpha){VK_FORMAT_R8G8B8A8_SRGB, true};

    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return (struct format_alpha){VK_FORMAT_B8G8R8A8_SRGB, true};
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        return (struct format_alpha){VK_FORMAT_B8G8R8A8_SRGB, false};

    // Legacy, see:
    // https://github.com/leadedge/Spout2/blob/2.007.017/SPOUTSDK/SpoutGL/SpoutDirectX.cpp#L580
    case 0:
        // VSeeFace uses RGBA order here? Might need to peek at DX texture
        // format...
        return (struct format_alpha){VK_FORMAT_R8G8B8A8_SRGB, true};
    case D3DFMT_A8R8G8B8:
        return (struct format_alpha){VK_FORMAT_B8G8R8A8_SRGB, true};
    case D3DFMT_X8R8G8B8:
        return (struct format_alpha){VK_FORMAT_B8G8R8A8_SRGB, false};

    default:
        ERR("Unsupported DX format %d\n", format);
        return (struct format_alpha){VK_FORMAT_UNDEFINED, false};
    }
}

static int import_texture(struct source *source) {
    VkResult result;
    int fd = -1;

    if (source->info.opaque_fd < 0)
        return -EINVAL;

    if (source->cur_fd != -1) {
        close(source->cur_fd);
        source->cur_fd = -1;
    }

    fd = fcntl(source->info.opaque_fd, F_DUPFD_CLOEXEC, 3);
    if (fd < 0)
        return -EINVAL;

    source->cur_fd = source->info.opaque_fd;
    source->info.opaque_fd = -1;

    TRACE("Importing OPAQUE FD %d -> %d (%dx%d)\n", source->cur_fd, fd,
          source->info.width, source->info.height);

    struct format_alpha fmt_alpha = dx_to_vkformat(source->info.format);

    if (fmt_alpha.format == VK_FORMAT_UNDEFINED)
        goto err_close;

    VkExternalMemoryImageCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };

    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &create_info,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt_alpha.format,
        .extent = (VkExtent3D){source->info.width, source->info.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = 1,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    /**
     * Extends the usage of the image based on DirectX bind flags.
     * This maters on NVIDIA proprietary drivers on pre-Turing GPUs as this
     * seems to have interactions with caches. This follows
     * https://github.com/doitsujin/dxvk/blob/0bf876eb96767b3548aff3b27985f08d819bcd99/src/d3d11/d3d11_texture.cpp#L96
     */
    if (source->info.bind_flags & D3D11_BIND_SHADER_RESOURCE)
        info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (source->info.bind_flags & D3D11_BIND_RENDER_TARGET)
        info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (source->info.bind_flags & D3D11_BIND_UNORDERED_ACCESS)
        info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;

    CHECK_VK_RESULT(vkCreateImage(device, &info, NULL, &source->image)) {
        goto err_close;
    }

    const VkImageMemoryRequirementsInfo2 mem_reqs_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = source->image,
    };
    VkMemoryRequirements2 mem_reqs = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
    };

    vk.vkGetImageMemoryRequirements2KHR(device, &mem_reqs_info, &mem_reqs);

    uint32_t memory_type_bits = mem_reqs.memoryRequirements.memoryTypeBits;

    if (preferredMemoryTypeBits & memory_type_bits)
        memory_type_bits &= preferredMemoryTypeBits;

    TRACE("Memory type bits: required=0x%x, preferred=0x%x, choices=0x%x\n",
          mem_reqs.memoryRequirements.memoryTypeBits, preferredMemoryTypeBits,
          memory_type_bits);

    if (!memory_type_bits) {
        ERR("No valid memory type\n");
        goto err_close;
    }

    VkImportMemoryFdInfoKHR memory_fd_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .fd = fd,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };

    VkMemoryAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &memory_fd_info,
        .allocationSize = mem_reqs.memoryRequirements.size,
        .memoryTypeIndex = ffs(memory_type_bits) - 1,
    };

    if (source->info.resource_size) {
        if (allocate_info.allocationSize != source->info.resource_size) {
            ERR("resource size mismatch!");
        }
        allocate_info.allocationSize = source->info.resource_size;
    }

    CHECK_VK_RESULT(
        vkAllocateMemory(device, &allocate_info, NULL, &source->mem)) {
        goto err_close;
    }
    fd = -1;

    CHECK_VK_RESULT(vkBindImageMemory(device, source->image, source->mem, 0)) {
        return -EINVAL;
    }

    TRACE("Texture import OK\n");

    return 0;

err_close:
    if (source->cur_fd != -1)
        close(source->cur_fd);
    source->cur_fd = -1;
    if (fd != -1)
        close(fd);
    return -EINVAL;
}

static NTSTATUS run_source(void *args) {
    VkResult result = VK_SUCCESS;

    struct source *source = args;
    bool active = false;
    int ret;

    TRACE("run_source()\n");

    pthread_mutex_lock(&source->lock);
    while (!source->quit) {
        TRACE("run_source(): Iterate\n");
        if (source->update) {
            TRACE("run_source(): Update flags=%d\n", source->info.flags);
            source->update = false;
            if (source->info.flags &
                (RECEIVER_DISCONNECTED | RECEIVER_TEXTURE_INVALID)) {
                TRACE("run_source(): Inactive\n");
                active = false;
            } else if (source->info.flags & RECEIVER_TEXTURE_UPDATED) {
                free_texture(source);
                if (import_texture(source) == 0) {
                    ret = funnel_stream_set_size(source->stream,
                                                 source->info.width,
                                                 source->info.height);
                    if (ret) {
                        ERR("Failed to set size\n");
                        continue;
                    }
                    ret = funnel_stream_configure(source->stream);
                    if (ret) {
                        ERR("Failed to configure stream\n");
                        continue;
                    }
                    ret = funnel_stream_start(source->stream);
                    if (ret) {
                        ERR("Failed to start stream\n");
                        continue;
                    }
                    source->width = source->info.width;
                    source->height = source->info.height;
                    active = true;
                } else {
                    ERR("Texture import failed, stopping stream\n");
                    active = false;
                }
            }
        }
        if (!active) {
            TRACE("run_source(): Stop\n");
            funnel_stream_stop(source->stream);
            free_texture(source);
            pthread_cond_wait(&source->cond, &source->lock);
            continue;
        }
        pthread_mutex_unlock(&source->lock);

        TRACE("run_source(): Dequeuing\n");

        struct funnel_buffer *buf = NULL;
        ret = funnel_stream_dequeue(source->stream, &buf);
        if (ret < 0) {
            ERR("Buffer dequeue failed: %d\n", ret);
            goto cont;
        }
        if (ret == 0) {
            TRACE("No buffer\n");
            goto cont;
        }

        uint32_t bwidth, bheight;
        funnel_buffer_get_size(buf, &bwidth, &bheight);
        if (bwidth != source->width || bheight != source->height) {
            TRACE("Dimensions mismatch, skipping buffer\n");
            funnel_stream_return(source->stream, buf);
            goto cont;
        }

        VkSemaphore acquire, release;
        ret = funnel_buffer_get_vk_semaphores(buf, &acquire, &release);
        if (ret) {
            ERR("Failed to get semaphores: %d\n", ret);
            funnel_stream_return(source->stream, buf);
            goto cont;
        }

        VkFence fence;
        ret = funnel_buffer_get_vk_fence(buf, &fence);
        if (ret) {
            ERR("Failed to get fence: %d\n", ret);
            funnel_stream_return(source->stream, buf);
            goto cont;
        }

        VkImage image;
        ret = funnel_buffer_get_vk_image(buf, &image);
        if (ret) {
            ERR("Failed to get image: %d\n", ret);
            funnel_stream_return(source->stream, buf);
            goto cont;
        }
        assert(image);

        VkCommandBufferBeginInfo beginInfo = {0};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        CHECK_VK_RESULT(
            vkBeginCommandBuffer(source->commandBuffer, &beginInfo)) {
            ERR("Failed to init command buffer\n");
            funnel_stream_return(source->stream, buf);
            goto cont;
        }

        VkImageBlit region = {
            .srcSubresource =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            .srcOffsets = {{0, 0, 0}, {bwidth, bheight, 1}},
            .dstSubresource =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            .dstOffsets = {{0, 0, 0}, {bwidth, bheight, 1}},
        };

        vkCmdBlitImage(source->commandBuffer, source->image,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                       VK_FILTER_NEAREST);

        CHECK_VK_RESULT(vkEndCommandBuffer(source->commandBuffer)) {
            ERR("Failed to end command buffer\n");
            funnel_stream_return(source->stream, buf);
            goto cont;
        }

        const VkPipelineStageFlags waitStage =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSubmitInfo submitInfo = {0};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &acquire;
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &source->commandBuffer;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &release;

        struct lock_texture_return *ltex = lock_texture(source->receiver);

        if (!ltex) {
            ERR("Failed to lock texture\n");
            funnel_stream_return(source->stream, buf);
            goto cont;
        }

        TRACE("run_source(): Submitting\n");

        CHECK_VK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, fence)) {
            unlock_texture(source->receiver);
            funnel_stream_return(source->stream, buf);
            goto cont;
        }

        TRACE("run_source(): Enqueueing\n");

        ret = funnel_stream_enqueue(source->stream, buf);
        if (ret < 0) {
            ERR("Enqueue failed: %d\n", ret);
            funnel_stream_return(source->stream, buf);
        }

        TRACE("run_source(): Wait for idle\n");

        CHECK_VK_RESULT(vkQueueWaitIdle(queue)) {}

        TRACE("run_source(): Unlock\n");

        unlock_texture(source->receiver);

    cont:
        pthread_mutex_lock(&source->lock);
        if (result != VK_SUCCESS) {
            ERR("Vulkan error (device lost?), stopping source permanently\n");
            break;
        }
    }

    TRACE("run_source(): exiting\n");

    vkFreeCommandBuffers(device, commandPool, 1, &source->commandBuffer);

    if (source->info.opaque_fd != -1) {
        close(source->info.opaque_fd);
        source->info.opaque_fd = -1;
    }

    free_texture(source);
    funnel_stream_stop(source->stream);
    funnel_stream_destroy(source->stream);

    source->dead = true;

    while (!source->quit)
        pthread_cond_wait(&source->cond, &source->lock);

    pthread_mutex_unlock(&source->lock);
    pthread_cond_destroy(&source->cond);
    pthread_mutex_destroy(&source->lock);
    free(source);

    TRACE("run_source(): exit\n");

    return STATUS_SUCCESS;
}

static NTSTATUS update_source(void *args) {
    struct update_source_params *params = args;
    struct source *source = params->source;

    pthread_mutex_lock(&source->lock);

    if (source->dead) {
        pthread_mutex_unlock(&source->lock);
        return STATUS_NO_SUCH_DEVICE;
    }

    if (source->info.opaque_fd != -1) {
        close(source->info.opaque_fd);
        source->info.opaque_fd = -1;
    }
    source->info = params->info;
    source->update = true;
    pthread_cond_broadcast(&source->cond);

    pthread_mutex_unlock(&source->lock);

    return STATUS_SUCCESS;
}

static NTSTATUS destroy_source(void *args) {
    struct source *source = args;

    pthread_mutex_lock(&source->lock);
    source->quit = true;
    pthread_cond_broadcast(&source->cond);
    if (!source->dead)
        funnel_stream_skip_frame(source->stream);
    pthread_mutex_unlock(&source->lock);

    // Freed when the thread exits

    return STATUS_SUCCESS;
}

static void teardown(void) {
    funnel_shutdown(funnel);

    vkDestroyCommandPool(device, commandPool, NULL);
    vkDestroyDevice(device, NULL);
    GET_EXTENSION_FUNCTION(vkDestroyDebugUtilsMessengerEXT)(
        instance, debugMessenger, NULL);
    vkDestroyInstance(instance, NULL);

    WINE_TRACE("Teardown finished\n");
}

static NTSTATUS _teardown(void *args) {
    teardown();

    return STATUS_SUCCESS;
}

static NTSTATUS libs_init(void *args) { return STATUS_SUCCESS; }

const unixlib_entry_t __wine_unix_call_funcs[] = {
    libs_init,  startup,       _teardown,      create_source,
    run_source, update_source, destroy_source,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count);
