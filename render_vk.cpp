/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "render_vk.h"

#include "cell_extra_store.h"
#include "composer.h"
#include "font_pack.h"
#include "listener.h"
#include "render.h"

#include "options.h"
#include "render_damage.h"
#include "screen.h"
#include "utf8.h"
#include "vterm.h"

#include <plt/window.h>

#include <std/dbg/assert.h>
#include <std/alg/xchg.h>
#include <std/sys/crt.h>
#include <std/ios/sys.h>
#include <std/lib/buffer.h>
#include <std/lib/list.h>
#include <std/lib/vector.h>
#include <std/mem/new.h>
#include <std/mem/obj_pool.h>
#include <std/mem/small_obj_allocator.h>
#include <std/rng/mix.h>
#include <std/str/hash.h>
#include <std/str/view.h>
#include <std/typ/intrin.h>

#include <vulkan/vulkan.h>

#if defined(HAVE_VULKAN_WAYLAND)
    #include <vulkan/vulkan_wayland.h>
#else
    #error No Vulkan window-system backend selected
#endif

#include "render_spv.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace stl;

namespace {
    #if defined(VK_KHR_swapchain_maintenance1)
        using SwapchainMaintenanceFeatures = VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR;
        using SwapchainPresentFenceInfo = VkSwapchainPresentFenceInfoKHR;
        constexpr VkStructureType swapchainMaintenanceFeaturesType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR;
        constexpr VkStructureType swapchainPresentFenceInfoType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR;
    #else
        using SwapchainMaintenanceFeatures = VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT;
        using SwapchainPresentFenceInfo = VkSwapchainPresentFenceInfoEXT;
        constexpr VkStructureType swapchainMaintenanceFeaturesType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
        constexpr VkStructureType swapchainPresentFenceInfoType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT;
    #endif

    struct RendererImpl;

    struct CallRendererFontChanged final: public Listener {
        explicit CallRendererFontChanged(RendererImpl* renderer);

        void onListen(void*) override;

        RendererImpl* renderer;
    };

    // strip: the pixel offset of the cell's slice base in its plane's
    // arena, the top bit selecting the color plane; stripNone marks a
    // cell with no strip (blank, or coverage the shader synthesizes).
    constexpr u32 stripNone = 0xffffffffu;
    constexpr u32 stripColorPlane = 0x80000000u;

    struct GpuCell {
        u32 codepoint = ' ';
        u32 attributes = 0;
        u32 foreground = 0;
        u32 background = 0;
        u32 underlineColor = 0;
        u32 hyperlink = 0;
        u32 strip = stripNone;
        u32 stripStride = 0;
        u32 semantic = 0;
        u32 lineAttribute = 0;
    };

    static_assert(sizeof(GpuCell) == 40, "Vulkan cell layout mismatch");

    constexpr u32 gpuBold = 1u << 2;
    constexpr u32 gpuItalic = 1u << 3;
    constexpr u32 gpuUnderline = 1u << 4;
    constexpr u32 gpuInverse = 1u << 5;
    constexpr u32 gpuWrap = 1u << 6;
    constexpr u32 gpuFaint = 1u << 8;
    constexpr u32 gpuBlink = 1u << 9;
    constexpr u32 gpuConceal = 1u << 10;
    constexpr u32 gpuStrike = 1u << 11;
    constexpr u32 gpuOverline = 1u << 12;
    constexpr u32 gpuUnderlineStyle = 0x7u << 13;
    constexpr u32 gpuDoubleWidth = 1u << 16;
    constexpr u32 gpuDoubleWidthContinuation = 1u << 17;
    constexpr u32 gpuProtection = 0x3u << 18;
    constexpr u32 gpuDrawn = 1u << 20;

    struct GpuCellUpdate {
        u32 sourceIndex;
        u32 outputIndex;
        GpuCell cell;
    };

    static_assert(sizeof(GpuCellUpdate) == 48, "Vulkan cell update layout mismatch");

    struct RendererImpl final: public Renderer {
        RendererImpl(Composer& composer, const plt::RenderContext& context);
        ~RendererImpl();

        bool update(const TerminalUpdate& update) override;
        bool repaint() override;
        bool repaintFrame();

        struct ImageResource {
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            u32 width = 0;
            u32 height = 0;
            u32 layers = 0;
        };

        struct FrameResources {
            VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
            VkBuffer cellBuffer = VK_NULL_HANDLE;
            VkDeviceMemory cellMemory = VK_NULL_HANDLE;
            void* cells = nullptr;
            size_t cellCapacity = 0;
            VkBuffer fontUploadBuffer = VK_NULL_HANDLE;
            VkDeviceMemory fontUploadMemory = VK_NULL_HANDLE;
            void* fontUploads = nullptr;
            size_t fontUploadCapacity = 0;
            VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
            VkSemaphore imageAvailable = VK_NULL_HANDLE;
            VkFence fence = VK_NULL_HANDLE;
        };

        struct PushConstants {
            u32 glyphWidth;
            u32 glyphHeight;
            u32 columns;
            u32 rows;
            u32 outputWidth;
            u32 outputHeight;
            u32 border;
            u32 cursorColor;
            i32 cursorX;
            i32 cursorY;
            u32 cursorStyle;
            u32 screenReverseVideo;
            i32 selectionLeft;
            i32 selectionTop;
            i32 selectionRight;
            i32 selectionBottom;
            u32 rectangularSelection;
            u32 showWraps;
            u32 selectionForeground;
            u32 selectionBackground;
            u32 selectionColorMask;
            u32 blinkVisible;
            u32 cursorBlink;
            u32 hoveredHyperlink;
            u32 hoveredLinkBegin;
            u32 hoveredLinkEnd;
            u32 updateCount;
        };

        static_assert(sizeof(PushConstants) == 108, "Vulkan push constant layout mismatch");

        // The strip arenas mirrored on the device; append-only between
        // collections, so only the tail uploads each frame.
        struct ArenaBuffer {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkDeviceSize capacity = 0;
            size_t uploaded = 0;
        };

        struct FontResources {
            ArenaBuffer mask;
            ArenaBuffer color;
        };

        struct SwapchainResources {
            VkSwapchainKHR swapchain = VK_NULL_HANDLE;
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkFormat storageViewFormat = VK_FORMAT_UNDEFINED;
            VkExtent2D extent{};
            Vector<VkImage> images;
            Vector<VkImageView> views;
            Vector<VkSemaphore> semaphores;
            Vector<VkFence> presentFences;
            Vector<u8> presentFencePending;
            Vector<u8> initialized;
            Vector<u64> generations;
            ImageResource output;
            bool outputInitialized = false;
            u64 outputGeneration = 0;
            bool direct = false;
            // Without swapchain maintenance there is no presentation fence:
            // destroy after this many further presented frames instead of
            // piling retirees up to a device-wide wait.
            u32 gracePresents = 0;
        };

        struct PresentTarget {
            VkSurfaceFormatKHR format{};
            const GeneratedRenderShader* shader = nullptr;
            bool direct = false;
        };

        struct PresentationState {
            TerminalCursor cursor;
            Rect selection;
            Color selectionForeground;
            Color selectionBackground;
            u32 selectionColorMask = 0;
            u32 hoveredHyperlink = 0;
            u32 hoveredLinkBegin = 0;
            u32 hoveredLinkEnd = 0;
            bool screenReverse = false;
            bool blinkVisible = false;
            bool cursorBlink = false;
        };

        static constexpr u32 framesInFlight = 2;

        Composer& composer;

        VkInstance instance = VK_NULL_HANDLE;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        u32 queueFamily = UINT32_MAX;
        VkQueue queue = VK_NULL_HANDLE;
        VkCommandPool commandPool = VK_NULL_HANDLE;

        VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        const GeneratedRenderShader* activeShader = nullptr;

        // The live arena buffers; never null after construction.
        // resetFontResources installs a replacement by pointer swap.
        FontResources* fontResources = nullptr;
        // The arena generation the device copies mirror; a mismatch means
        // the strips moved wholesale and everything re-uploads.
        u32 stripGeneration = 0;
        Buffer fontUploadData;
        Buffer updateEpochs;
        u32 updateEpoch = 0;
        Buffer damageJournalStorage;
        RenderDamage damage;
        u64 clearDamageGeneration = 0;
        Vector<GpuCell> cells;
        Vector<VkBufferCopy> maskArenaCopies;
        Vector<VkBufferCopy> colorArenaCopies;
        Vector<ScreenRowSpan> spanScratch;
        TerminalCursor previousCursor;
        Rect previousSelection;
        Color clearBackground = opts.bg;
        u32 previousHoveredHyperlink = 0;
        u32 previousHoveredLinkBegin = 0;
        u32 previousHoveredLinkEnd = 0;
        bool previousStateValid = false;
        PresentationState presentationState;
        u16 cellColumns = 0;
        u16 cellRows = 0;
        bool mutableSwapchainFormats = false;
        bool extendedStorageFormats = false;
        bool khrSurfaceMaintenance = false;
        bool extSurfaceMaintenance = false;
        const char* swapchainMaintenanceExtension = nullptr;

        // The live presentation chain; never null. Replaced wholesale by
        // createSwapchain, which retires the previous instance.
        SwapchainResources* chain = nullptr;
        VkExtent2D renderExtent{};
        Vector<SwapchainResources*> retiredSwapchains;
        Vector<VkPipeline> retiredPipelines;

        std::array<FrameResources, framesInFlight> frames;
        u32 currentFrame = 0;
        void createInstance();
        void createSurface(const plt::RenderContext& context);
        void selectPhysicalDevice();
        void createDevice();
        void createCommandResources();
        void createFontResources();
        FontResources* buildFontResources();
        void destroyFontResources(FontResources& resources);
        void resetFontResources();
        void createDescriptors();
        void createPipelineLayout();
        void selectPipeline(const GeneratedRenderShader& shader);
        PresentTarget selectPresentTarget(const VkSurfaceCapabilitiesKHR& capabilities, const std::vector<VkSurfaceFormatKHR>& formats) const;
        void createSwapchain(u32 width, u32 height);
        bool tryCreateSwapchain(u32 width, u32 height);
        void destroySwapchainResources(SwapchainResources& resources);
        void retireSwapchain(SwapchainResources* resources);
        void collectRetiredSwapchains(bool force = false);
        void ensureCellBuffer(FrameResources& frame, size_t bytes);
        void ensureFontUploadBuffer(FrameResources& frame, size_t bytes);
        void releaseBuffer(VkBuffer& buffer, VkDeviceMemory& memory, void*& mapped);

        ImageResource createImage(u32 width, u32 height, u32 layers, VkFormat format, VkImageUsageFlags usage, bool arrayView = false, VkFormat viewFormat = VK_FORMAT_UNDEFINED);
        void destroyImage(ImageResource& image);
        VkImageView createImageView(VkImage image, VkFormat format) const;
        void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& memory) const;
        u32 findMemoryType(u32 allowed, VkMemoryPropertyFlags properties) const;
        void updateStaticDescriptors();
        void updateOutputDescriptor(FrameResources& frame, VkImageView view);
        void updateCellDescriptor(FrameResources& frame);
        VkDeviceSize stageFontData(const void* data, size_t len, size_t expected);
        void ensureArenaBuffer(ArenaBuffer& arena, size_t bytes);
        void stageArenaCopy(ArenaBuffer& arena, Vector<VkBufferCopy>& copies, const u8* data, size_t used);
        void stageArenaTails(Screen& shapes, u32 generation);
        void applySpanStrips(u32 rowIndex, const ScreenRowSpan& span);
        void assignRowStrips(Screen& shapes, u16 row);
        void overrideOverlayStrips(Screen& shapes, const TerminalUpdate& update);
        u32 assignStrips(const TerminalUpdate& update, bool allRows);
        void resetArenaStaging();
        void recordArenaUploads(FrameResources& frame);
        void recordCommands(FrameResources& frame, u32 imageIndex, const PresentationState& state, u32 updateCount, bool clearOutput);
        void recordRepaintCommands(FrameResources& frame, u32 imageIndex);
        void recordBlit(FrameResources& frame, u32 imageIndex, VkAccessFlags outputSrcAccess, VkPipelineStageFlags outputSrcStage);
        void recordFrame(FrameResources& frame, u32 imageIndex);
        bool acquirePresentFrame(u32 width, u32 height, FrameResources*& frame, u32& imageIndex, bool& recreateAfterPresent);
        bool submitPresentFrame(u32 width, u32 height, FrameResources& frame, u32 imageIndex, bool recreateAfterPresent);
        bool present(const TerminalUpdate& update);
        u32 materializeUpdates(FrameResources& frame, u64 appliedGeneration, bool initialized);
        void materializeCells(const TerminalCell* input, GpuCell* output, u16 count, u8 lineAttribute, const TerminalColors& colors);
        void ensureDamageJournal(u32 rowCount);
        void appendDamage(u32 firstRow, u32 rowCount);
        void fullDamage();
        void collectDamage();
        void capturePresentationState(const TerminalUpdate& update);

        static u32 packColor(const Color& color);
        static bool sameSelection(const Rect& lhs, const Rect& rhs);
    };

    // Thrown where VK_ERROR_SURFACE_LOST_KHR surfaces; caught only at the
    // update()/repaint() boundary, where the renderer dies with its pool.
    struct SurfaceLost {};

    [[noreturn]] void failVk(const char* operation, VkResult result) {
        throw std::runtime_error(std::string(operation) + " failed (VkResult " + std::to_string((int)(result)) + ")");
    }

    void checkVk(VkResult result, const char* operation) {
        if (result != VK_SUCCESS) {
            failVk(operation, result);
        }
    }

    VkImageSubresourceRange imageRange(u32 layers) {
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = layers;
        return range;
    }

    void imageBarrier(VkCommandBuffer commandBuffer, VkImage image, u32 layers, VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkImageLayout oldLayout, VkImageLayout newLayout, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = srcAccess;
        barrier.dstAccessMask = dstAccess;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = imageRange(layers);
        vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    void bufferBarrier(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize size, VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
        VkBufferMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcAccessMask = srcAccess;
        barrier.dstAccessMask = dstAccess;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = buffer;
        barrier.size = size;
        vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, nullptr, 1, &barrier, 0, nullptr);
    }

    bool instanceHasExtension(const char* name) {
        u32 count = 0;
        if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) {
            return false;
        }

        Vector<VkExtensionProperties> extensions(count);
        if (vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.mutData()) != VK_SUCCESS) {
            return false;
        }
        for (u32 index = 0; index < count; ++index) {
            if (StringView(extensions[index].extensionName) == StringView(name)) {
                return true;
            }
        }
        return false;
    }

    bool deviceHasExtension(VkPhysicalDevice physicalDevice, const char* name) {
        u32 count = 0;
        if (vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr) != VK_SUCCESS) {
            return false;
        }

        Vector<VkExtensionProperties> extensions(count);
        if (vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, extensions.mutData()) != VK_SUCCESS) {
            return false;
        }
        for (u32 index = 0; index < count; ++index) {
            if (StringView(extensions[index].extensionName) == StringView(name)) {
                return true;
            }
        }
        return false;
    }

    bool formatSupports(VkPhysicalDevice physicalDevice, VkFormat format, VkFormatFeatureFlags features) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);
        return (properties.optimalTilingFeatures & features) == features;
    }

    bool deviceSupportsRenderer(VkPhysicalDevice physicalDevice) {
        return formatSupports(physicalDevice, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) && formatSupports(physicalDevice, VK_FORMAT_R8_UNORM, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
    }

    VkCompositeAlphaFlagBitsKHR selectCompositeAlpha(VkCompositeAlphaFlagsKHR supported) {
        const VkCompositeAlphaFlagBitsKHR choices[] = {
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        };
        for (const auto choice : choices) {
            if (supported & choice) {
                return choice;
            }
        }
        throw std::runtime_error("Vulkan surface has no composite alpha mode");
    }

}

CallRendererFontChanged::CallRendererFontChanged(RendererImpl* renderer_)
    : renderer(renderer_)
{
}

void CallRendererFontChanged::onListen(void*) {
    renderer->resetFontResources();
}

RendererImpl::RendererImpl(Composer& composer_, const plt::RenderContext& context)
    : composer(composer_)
{
    chain = composer.smallObjects->make<SwapchainResources>();
    createInstance();
    createSurface(context);
    selectPhysicalDevice();
    createDevice();
    createCommandResources();
    createFontResources();
    createDescriptors();
    createPipelineLayout();
}

RendererImpl::~RendererImpl() {
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
    }

    destroySwapchainResources(*chain);
    composer.smallObjects->release(chain);
    collectRetiredSwapchains(true);
    if (pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, pipeline, nullptr);
    }
    for (const VkPipeline retired : retiredPipelines) {
        vkDestroyPipeline(device, retired, nullptr);
    }
    if (pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    }
    if (descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, descriptorPool, nullptr);
    }
    if (descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
    }

    destroyFontResources(*fontResources);
    composer.smallObjects->release(fontResources);

    for (auto& frame : frames) {
        releaseBuffer(frame.cellBuffer, frame.cellMemory, frame.cells);
        releaseBuffer(frame.fontUploadBuffer, frame.fontUploadMemory, frame.fontUploads);
        if (frame.imageAvailable != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, frame.imageAvailable, nullptr);
        }
        if (frame.fence != VK_NULL_HANDLE) {
            vkDestroyFence(device, frame.fence, nullptr);
        }
    }

    if (commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, commandPool, nullptr);
    }
    if (device != VK_NULL_HANDLE) {
        vkDestroyDevice(device, nullptr);
    }
    if (surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance, surface, nullptr);
    }
    if (instance != VK_NULL_HANDLE) {
        vkDestroyInstance(instance, nullptr);
    }
}

void RendererImpl::createInstance() {
    const char* extensions[5] = {VK_KHR_SURFACE_EXTENSION_NAME};
    u32 extensionCount = 1;
    extensions[extensionCount++] = VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
    #if defined(VK_KHR_surface_maintenance1)
        khrSurfaceMaintenance = instanceHasExtension(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    #endif
    extSurfaceMaintenance = instanceHasExtension(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    #if defined(VK_KHR_surface_maintenance1)
        if (khrSurfaceMaintenance) {
            extensions[extensionCount++] = VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME;
        }
    #endif
    if (extSurfaceMaintenance) {
        extensions[extensionCount++] = VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME;
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "shitty";
    appInfo.applicationVersion = 0;
    appInfo.pEngineName = "shitty";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 2, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = extensionCount;
    createInfo.ppEnabledExtensionNames = extensions;
    checkVk(vkCreateInstance(&createInfo, nullptr, &instance), "vkCreateInstance");
}

void RendererImpl::createSurface(const plt::RenderContext& context) {
    if (context.backend != plt::RenderBackend::Wayland || context.connection == nullptr || context.window == nullptr) {
        throw std::runtime_error("Vulkan renderer requires a Wayland render context");
    }
    VkWaylandSurfaceCreateInfoKHR surfaceInfo{};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.display = (struct wl_display*)(context.connection);
    surfaceInfo.surface = (struct wl_surface*)(context.window);
    checkVk(vkCreateWaylandSurfaceKHR(instance, &surfaceInfo, nullptr, &surface), "vkCreateWaylandSurfaceKHR");
}

void RendererImpl::selectPhysicalDevice() {
    u32 deviceCount = 0;
    checkVk(vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr), "vkEnumeratePhysicalDevices");
    if (deviceCount == 0) {
        throw std::runtime_error("No Vulkan physical devices found");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    checkVk(vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data()), "vkEnumeratePhysicalDevices");

    int bestScore = -1;
    VkPhysicalDeviceProperties bestProperties{};
    for (const auto candidate : devices) {
        if (!deviceHasExtension(candidate, VK_KHR_SWAPCHAIN_EXTENSION_NAME) || !deviceSupportsRenderer(candidate)) {
            continue;
        }

        u32 familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());

        for (u32 family = 0; family < familyCount; ++family) {
            VkBool32 presentSupported = VK_FALSE;
            checkVk(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, family, surface, &presentSupported), "vkGetPhysicalDeviceSurfaceSupportKHR");
            const VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if ((families[family].queueFlags & required) != required || !presentSupported) {
                continue;
            }

            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            const int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100 : properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 50 : 10;
            if (score > bestScore) {
                bestScore = score;
                physicalDevice = candidate;
                queueFamily = family;
                bestProperties = properties;
            }
        }
    }

    if (physicalDevice == VK_NULL_HANDLE) {
        throw std::runtime_error(
            "No Vulkan device supports compute rendering and window-system "
            "presentation"
        );
    }

    if (opts.vulkanInfo) {
        sysO << StringView(u8"Vulkan device: ") << StringView(bestProperties.deviceName) << StringView(u8"\nVulkan API: ") << (u64)(VK_VERSION_MAJOR(bestProperties.apiVersion)) << StringView(u8".") << (u64)(VK_VERSION_MINOR(bestProperties.apiVersion)) << StringView(u8".") << (u64)(VK_VERSION_PATCH(bestProperties.apiVersion)) << endL;
    }

    mutableSwapchainFormats = deviceHasExtension(physicalDevice, VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME) && deviceHasExtension(physicalDevice, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(physicalDevice, &features);
    extendedStorageFormats = features.shaderStorageImageExtendedFormats;

    #if defined(VK_KHR_swapchain_maintenance1)
        if (khrSurfaceMaintenance && deviceHasExtension(physicalDevice, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) {
            swapchainMaintenanceExtension = VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
        } else
    #endif
    if (extSurfaceMaintenance && deviceHasExtension(physicalDevice, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) {
        swapchainMaintenanceExtension = VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
    }
    if (swapchainMaintenanceExtension != nullptr) {
        SwapchainMaintenanceFeatures maintenance{};
        maintenance.sType = swapchainMaintenanceFeaturesType;
        VkPhysicalDeviceFeatures2 available{};
        available.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        available.pNext = &maintenance;
        vkGetPhysicalDeviceFeatures2(physicalDevice, &available);
        if (!maintenance.swapchainMaintenance1) {
            swapchainMaintenanceExtension = nullptr;
        }
    }
}

void RendererImpl::createDevice() {
    constexpr float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    const char* extensions[5] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    u32 extensionCount = 1;
    if (mutableSwapchainFormats) {
        extensions[extensionCount++] = VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME;
        extensions[extensionCount++] = VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME;
    }
    constexpr const char* portabilitySubset = "VK_KHR_portability_subset";
    if (deviceHasExtension(physicalDevice, portabilitySubset)) {
        extensions[extensionCount++] = portabilitySubset;
    }
    if (swapchainMaintenanceExtension != nullptr) {
        extensions[extensionCount++] = swapchainMaintenanceExtension;
    }
    VkPhysicalDeviceFeatures features{};
    features.shaderStorageImageExtendedFormats = extendedStorageFormats;
    SwapchainMaintenanceFeatures maintenance{};
    if (swapchainMaintenanceExtension != nullptr) {
        maintenance.sType = swapchainMaintenanceFeaturesType;
        maintenance.swapchainMaintenance1 = VK_TRUE;
    }
    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext = swapchainMaintenanceExtension == nullptr ? nullptr : &maintenance;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueInfo;
    createInfo.enabledExtensionCount = extensionCount;
    createInfo.ppEnabledExtensionNames = extensions;
    createInfo.pEnabledFeatures = &features;
    checkVk(vkCreateDevice(physicalDevice, &createInfo, nullptr, &device), "vkCreateDevice");
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
}

void RendererImpl::createCommandResources() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily;
    checkVk(vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool), "vkCreateCommandPool");

    std::array<VkCommandBuffer, framesInFlight> commandBuffers{};
    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = framesInFlight;
    checkVk(vkAllocateCommandBuffers(device, &allocateInfo, commandBuffers.data()), "vkAllocateCommandBuffers");

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (u32 i = 0; i < framesInFlight; ++i) {
        frames[i].commandBuffer = commandBuffers[i];
        checkVk(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &frames[i].imageAvailable), "vkCreateSemaphore");
        checkVk(vkCreateFence(device, &fenceInfo, nullptr, &frames[i].fence), "vkCreateFence");
    }
}

u32 RendererImpl::findMemoryType(u32 allowed, VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    for (u32 i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if ((allowed & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    throw std::runtime_error("No suitable Vulkan memory type found");
}

void RendererImpl::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& memory) const {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    checkVk(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer), "vkCreateBuffer");

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkMemoryAllocateInfo allocationInfo{};
    allocationInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocationInfo.allocationSize = requirements.size;
    allocationInfo.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, properties);
    checkVk(vkAllocateMemory(device, &allocationInfo, nullptr, &memory), "vkAllocateMemory");
    checkVk(vkBindBufferMemory(device, buffer, memory, 0), "vkBindBufferMemory");
}

RendererImpl::ImageResource RendererImpl::createImage(u32 width, u32 height, u32 layers, VkFormat format, VkImageUsageFlags usage, bool arrayView, VkFormat viewFormat) {
    if (viewFormat == VK_FORMAT_UNDEFINED) {
        viewFormat = format;
    }
    ImageResource result;
    result.width = width;
    result.height = height;
    result.layers = layers;

    try {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        if (viewFormat != format) {
            imageInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        }
        imageInfo.extent = {width, height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = layers;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = usage;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        checkVk(vkCreateImage(device, &imageInfo, nullptr, &result.image), "vkCreateImage");

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, result.image, &requirements);
        VkMemoryAllocateInfo allocationInfo{};
        allocationInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocationInfo.allocationSize = requirements.size;
        allocationInfo.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        checkVk(vkAllocateMemory(device, &allocationInfo, nullptr, &result.memory), "vkAllocateMemory");
        checkVk(vkBindImageMemory(device, result.image, result.memory, 0), "vkBindImageMemory");

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = result.image;
        viewInfo.viewType = arrayView ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = viewFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = layers;
        checkVk(vkCreateImageView(device, &viewInfo, nullptr, &result.view), "vkCreateImageView");
    } catch (...) {
        destroyImage(result);
        throw;
    }
    return result;
}

VkImageView RendererImpl::createImageView(VkImage image, VkFormat format) const {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    checkVk(vkCreateImageView(device, &viewInfo, nullptr, &view), "vkCreateImageView");
    return view;
}

void RendererImpl::destroyImage(ImageResource& image) {
    if (device != VK_NULL_HANDLE && image.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, image.view, nullptr);
    }
    if (device != VK_NULL_HANDLE && image.image != VK_NULL_HANDLE) {
        vkDestroyImage(device, image.image, nullptr);
    }
    if (device != VK_NULL_HANDLE && image.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, image.memory, nullptr);
    }
    image = {};
}

RendererImpl::FontResources* RendererImpl::buildFontResources() {
    FontResources* const resources = composer.smallObjects->make<FontResources>();
    try {
        // Descriptors always need valid buffers; real capacity arrives
        // with the first strips.
        createBuffer(4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, resources->mask.buffer, resources->mask.memory);
        resources->mask.capacity = 4;
        createBuffer(4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, resources->color.buffer, resources->color.memory);
        resources->color.capacity = 4;
    } catch (...) {
        destroyFontResources(*resources);
        composer.smallObjects->release(resources);
        throw;
    }
    return resources;
}

void RendererImpl::destroyFontResources(FontResources& resources) {
    if (resources.mask.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, resources.mask.buffer, nullptr);
        vkFreeMemory(device, resources.mask.memory, nullptr);
    }
    if (resources.color.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, resources.color.buffer, nullptr);
        vkFreeMemory(device, resources.color.memory, nullptr);
    }
}

void RendererImpl::createFontResources() {
    fontResources = buildFontResources();
}

void RendererImpl::resetFontResources() {
    FontResources* const replacement = buildFontResources();
    try {
        checkVk(vkDeviceWaitIdle(device), "vkDeviceWaitIdle");
    } catch (...) {
        destroyFontResources(*replacement);
        composer.smallObjects->release(replacement);
        throw;
    }

    FontResources* const previous = fontResources;
    fontResources = replacement;
    updateStaticDescriptors();
    destroyFontResources(*previous);
    composer.smallObjects->release(previous);
    resetArenaStaging();
    // The screen reset its arenas with the font; generation zero never
    // occurs there, so everything re-uploads on the next frame.
    stripGeneration = 0;
    previousStateValid = false;
}

void RendererImpl::createDescriptors() {
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    for (u32 binding = 1; binding < 4; ++binding) {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = bindings.size();
    layoutInfo.pBindings = bindings.data();
    checkVk(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descriptorSetLayout), "vkCreateDescriptorSetLayout");

    const VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, framesInFlight},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * framesInFlight},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = framesInFlight;
    poolInfo.poolSizeCount = std::size(poolSizes);
    poolInfo.pPoolSizes = poolSizes;
    checkVk(vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool), "vkCreateDescriptorPool");

    std::array<VkDescriptorSetLayout, framesInFlight> layouts;
    layouts.fill(descriptorSetLayout);
    std::array<VkDescriptorSet, framesInFlight> sets{};
    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = descriptorPool;
    allocateInfo.descriptorSetCount = framesInFlight;
    allocateInfo.pSetLayouts = layouts.data();
    checkVk(vkAllocateDescriptorSets(device, &allocateInfo, sets.data()), "vkAllocateDescriptorSets");
    for (u32 i = 0; i < framesInFlight; ++i) {
        frames[i].descriptorSet = sets[i];
    }
    updateStaticDescriptors();
}

void RendererImpl::updateStaticDescriptors() {
    for (auto& frame : frames) {
        const VkDescriptorBufferInfo bufferInfos[] = {
            {fontResources->mask.buffer, 0, VK_WHOLE_SIZE},
            {fontResources->color.buffer, 0, VK_WHOLE_SIZE},
        };
        std::array<VkWriteDescriptorSet, 2> writes{};
        for (u32 i = 0; i < writes.size(); ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = frame.descriptorSet;
            writes[i].dstBinding = i + 2;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &bufferInfos[i];
        }
        vkUpdateDescriptorSets(device, writes.size(), writes.data(), 0, nullptr);
    }
}

void RendererImpl::updateOutputDescriptor(FrameResources& frame, VkImageView view) {
    const VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = frame.descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

void RendererImpl::updateCellDescriptor(FrameResources& frame) {
    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = frame.cellBuffer;
    bufferInfo.range = frame.cellCapacity;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = frame.descriptorSet;
    write.dstBinding = 1;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

void RendererImpl::createPipelineLayout() {
    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstant.size = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;
    checkVk(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout), "vkCreatePipelineLayout");
}

void RendererImpl::selectPipeline(const GeneratedRenderShader& shader) {
    if (activeShader == &shader) {
        return;
    }
    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = shader.codeSize;
    moduleInfo.pCode = shader.code;
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    checkVk(vkCreateShaderModule(device, &moduleInfo, nullptr, &shaderModule), "vkCreateShaderModule");

    VkPipelineShaderStageCreateInfo shaderStage{};
    shaderStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    shaderStage.module = shaderModule;
    shaderStage.pName = "main";
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = shaderStage;
    pipelineInfo.layout = pipelineLayout;
    VkPipeline replacement = VK_NULL_HANDLE;
    const VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &replacement);
    vkDestroyShaderModule(device, shaderModule, nullptr);
    checkVk(result, "vkCreateComputePipelines");
    if (pipeline != VK_NULL_HANDLE) {
        retiredPipelines.pushBack(pipeline);
    }
    pipeline = replacement;
    activeShader = &shader;
}

void RendererImpl::destroySwapchainResources(SwapchainResources& resources) {
    for (const auto view : resources.views) {
        if (device != VK_NULL_HANDLE && view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, view, nullptr);
        }
    }
    for (const auto semaphore : resources.semaphores) {
        if (device != VK_NULL_HANDLE && semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, semaphore, nullptr);
        }
    }
    for (const VkFence fence : resources.presentFences) {
        if (device != VK_NULL_HANDLE && fence != VK_NULL_HANDLE) {
            vkDestroyFence(device, fence, nullptr);
        }
    }
    resources.views.clear();
    resources.semaphores.clear();
    resources.presentFences.clear();
    resources.presentFencePending.clear();
    resources.images.clear();
    resources.initialized.clear();
    resources.generations.clear();
    if (resources.swapchain != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, resources.swapchain, nullptr);
    }
    resources.swapchain = VK_NULL_HANDLE;
    resources.format = VK_FORMAT_UNDEFINED;
    resources.storageViewFormat = VK_FORMAT_UNDEFINED;
    resources.extent = {};
    destroyImage(resources.output);
    resources.outputInitialized = false;
    resources.outputGeneration = 0;
    resources.direct = false;
}

void RendererImpl::retireSwapchain(SwapchainResources* resources) {
    if (resources->swapchain == VK_NULL_HANDLE && resources->output.image == VK_NULL_HANDLE) {
        destroySwapchainResources(*resources);
        composer.smallObjects->release(resources);
        return;
    }
    if (swapchainMaintenanceExtension == nullptr) {
        resources->gracePresents = framesInFlight + 1;
    }
    retiredSwapchains.pushBack(resources);
    collectRetiredSwapchains();
    if (swapchainMaintenanceExtension == nullptr && retiredSwapchains.length() >= 8) {
        // No frame was presented since several retirements (a resize storm
        // with failing presents): fall back to a hard sync.
        checkVk(vkDeviceWaitIdle(device), "vkDeviceWaitIdle");
        collectRetiredSwapchains(true);
    }
}

void RendererImpl::collectRetiredSwapchains(bool force) {
    for (size_t index = 0; index != retiredSwapchains.length();) {
        SwapchainResources* const resources = retiredSwapchains[index];
        bool ready = force;
        if (!ready && swapchainMaintenanceExtension == nullptr) {
            // Own queue work is fenced framesInFlight frames later; the
            // extra frame is margin for the presentation engine, which is
            // unobservable without the maintenance extension.
            ready = resources->gracePresents == 0;
        }
        if (!ready && swapchainMaintenanceExtension != nullptr) {
            ready = true;
            for (size_t fenceIndex = 0; fenceIndex != resources->presentFences.length(); ++fenceIndex) {
                if (!resources->presentFencePending[fenceIndex]) {
                    continue;
                }
                const VkResult status = vkGetFenceStatus(device, resources->presentFences[fenceIndex]);
                if (status == VK_NOT_READY) {
                    ready = false;
                    break;
                }
                checkVk(status, "vkGetFenceStatus");
            }
        }
        if (!ready) {
            ++index;
            continue;
        }
        destroySwapchainResources(*resources);
        composer.smallObjects->release(resources);
        retiredSwapchains.mut(index) = retiredSwapchains.back();
        retiredSwapchains.popBack();
    }
}

RendererImpl::PresentTarget RendererImpl::selectPresentTarget(const VkSurfaceCapabilitiesKHR& capabilities, const std::vector<VkSurfaceFormatKHR>& formats) const {
    PresentTarget target;
    const VkImageUsageFlags directUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if ((capabilities.supportedUsageFlags & directUsage) == directUsage) {
        for (const GeneratedRenderShader& candidate : generatedRenderShaders) {
            if ((candidate.flags & renderShaderMutableFormat) && !mutableSwapchainFormats) {
                continue;
            }
            if ((candidate.flags & renderShaderExtendedStorage) && !extendedStorageFormats) {
                continue;
            }
            if (!formatSupports(physicalDevice, candidate.storageViewFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
                continue;
            }
            for (const VkSurfaceFormatKHR& format : formats) {
                if (format.format == candidate.presentFormat && format.colorSpace == candidate.colorSpace) {
                    target.format = format;
                    target.shader = &candidate;
                    break;
                }
            }
            if (target.shader != nullptr) {
                break;
            }
        }
    }

    target.direct = target.shader != nullptr;
    if (!target.direct) {
        if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            throw std::runtime_error("Vulkan surface supports neither storage output nor transfer destination");
        }
        const VkFormat preferredFormats[] = {
            VK_FORMAT_B8G8R8A8_UNORM,
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_FORMAT_B8G8R8A8_SRGB,
            VK_FORMAT_R8G8B8A8_SRGB,
        };
        for (const auto preferred : preferredFormats) {
            const auto found = std::find_if(formats.begin(), formats.end(), [this, preferred](const VkSurfaceFormatKHR& format) {
                // The shader produces sRGB-encoded bytes; other color
                // spaces would need a conversion the blit path lacks.
                return format.format == preferred && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR && formatSupports(physicalDevice, format.format, VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
            });
            if (found != formats.end()) {
                target.format = *found;
                break;
            }
        }
        if (target.format.format == VK_FORMAT_UNDEFINED) {
            throw std::runtime_error("Vulkan surface has no usable direct or blit format");
        }
        target.shader = &fallbackRenderShader;
    }

    if (target.direct && target.format.format != target.shader->storageViewFormat && !(target.shader->flags & renderShaderMutableFormat)) {
        throw std::runtime_error("Generated render shader requires an undeclared mutable format");
    }
    return target;
}

void RendererImpl::createSwapchain(u32 width, u32 height) {
    if (width == 0 || height == 0) {
        return;
    }

    VkSurfaceCapabilitiesKHR capabilities{};
    const VkResult capabilitiesResult = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities);
    if (capabilitiesResult == VK_ERROR_SURFACE_LOST_KHR) {
        throw SurfaceLost{};
    }
    checkVk(capabilitiesResult, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    u32 formatCount = 0;
    checkVk(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr), "vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (formatCount == 0) {
        throw std::runtime_error("Vulkan surface exposes no formats");
    }
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    checkVk(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, formats.data()), "vkGetPhysicalDeviceSurfaceFormatsKHR");

    const PresentTarget target = selectPresentTarget(capabilities, formats);
    const VkSurfaceFormatKHR surfaceFormat = target.format;
    const GeneratedRenderShader* const renderShader = target.shader;
    const bool direct = target.direct;

    u32 presentModeCount = 0;
    checkVk(vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, nullptr), "vkGetPhysicalDeviceSurfacePresentModesKHR");
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    checkVk(vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, presentModes.data()), "vkGetPhysicalDeviceSurfacePresentModesKHR");
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (std::find(presentModes.begin(), presentModes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != presentModes.end()) {
        presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    }

    VkExtent2D extent{};
    if (capabilities.currentExtent.width != std::numeric_limits<u32>::max()) {
        extent = capabilities.currentExtent;
    } else {
        extent.width = std::clamp(width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp(height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }

    u32 imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0) {
        imageCount = std::min(imageCount, capabilities.maxImageCount);
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = direct ? (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) : VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = selectCompositeAlpha(capabilities.supportedCompositeAlpha);
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = chain->swapchain;

    VkFormat viewFormats[2] = {
        surfaceFormat.format,
        renderShader->storageViewFormat,
    };
    VkImageFormatListCreateInfo formatList{};
    const bool mutableFormat = direct && (renderShader->flags & renderShaderMutableFormat);
    if (mutableFormat) {
        formatList.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
        formatList.viewFormatCount = viewFormats[0] == viewFormats[1] ? 1u : 2u;
        formatList.pViewFormats = viewFormats;
        createInfo.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
        createInfo.pNext = &formatList;
    }

    SwapchainResources* const replacement = composer.smallObjects->make<SwapchainResources>();
    replacement->format = surfaceFormat.format;
    replacement->storageViewFormat = renderShader->storageViewFormat;
    replacement->extent = extent;
    replacement->direct = direct;
    try {
        if (!direct) {
            // The shader stores already-sRGB-encoded bytes through a raw
            // UNORM view. Blitting a UNORM image into an sRGB swapchain
            // image would encode them a second time; giving the output
            // image an sRGB format makes the blit's decode+encode cancel
            // (and handles the channel order of BGRA targets).
            const bool srgbTarget = surfaceFormat.format == VK_FORMAT_R8G8B8A8_SRGB || surfaceFormat.format == VK_FORMAT_B8G8R8A8_SRGB;
            const VkFormat outputFormat = srgbTarget ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
            replacement->output = createImage(width, height, 1, outputFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false, VK_FORMAT_R8G8B8A8_UNORM);
        }
        checkVk(vkCreateSwapchainKHR(device, &createInfo, nullptr, &replacement->swapchain), "vkCreateSwapchainKHR");
        checkVk(vkGetSwapchainImagesKHR(device, replacement->swapchain, &imageCount, nullptr), "vkGetSwapchainImagesKHR");
        replacement->images.zero(imageCount);
        checkVk(vkGetSwapchainImagesKHR(device, replacement->swapchain, &imageCount, replacement->images.mutData()), "vkGetSwapchainImagesKHR");
        if (direct) {
            replacement->views.zero(imageCount);
            for (u32 index = 0; index < imageCount; ++index) {
                replacement->views.mut(index) = createImageView(replacement->images[index], replacement->storageViewFormat);
            }
        }
        replacement->semaphores.zero(imageCount);
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (VkSemaphore* semaphore = replacement->semaphores.mutBegin(); semaphore != replacement->semaphores.mutEnd(); ++semaphore) {
            checkVk(vkCreateSemaphore(device, &semaphoreInfo, nullptr, semaphore), "vkCreateSemaphore");
        }
        if (swapchainMaintenanceExtension != nullptr) {
            replacement->presentFences.zero(imageCount);
            replacement->presentFencePending.zero(imageCount);
            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            for (VkFence* fence = replacement->presentFences.mutBegin(); fence != replacement->presentFences.mutEnd(); ++fence) {
                checkVk(vkCreateFence(device, &fenceInfo, nullptr, fence), "vkCreateFence");
            }
        }
        replacement->initialized.zero(imageCount);
        replacement->generations.zero(imageCount);
        selectPipeline(*renderShader);
    } catch (...) {
        destroySwapchainResources(*replacement);
        composer.smallObjects->release(replacement);
        throw;
    }

    SwapchainResources* const previous = chain;
    chain = replacement;
    renderExtent = {width, height};
    if (opts.vulkanInfo) {
        sysO << StringView(u8"Vulkan presentation: ") << StringView(direct ? u8"direct storage (" : u8"offscreen blit (") << StringView(renderShader->name) << StringView(u8")") << endL;
    }
    retireSwapchain(previous);
}

bool RendererImpl::tryCreateSwapchain(u32 width, u32 height) {
    try {
        createSwapchain(width, height);
        return true;
    } catch (const SurfaceLost&) {
        throw;
    } catch (...) {
        return false;
    }
}

void RendererImpl::releaseBuffer(VkBuffer& buffer, VkDeviceMemory& memory, void*& mapped) {
    if (mapped != nullptr) {
        vkUnmapMemory(device, memory);
        mapped = nullptr;
    }
    if (buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
    }
    if (memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, memory, nullptr);
        memory = VK_NULL_HANDLE;
    }
}

void RendererImpl::ensureCellBuffer(FrameResources& frame, size_t bytes) {
    if (frame.cellCapacity >= bytes) {
        return;
    }

    releaseBuffer(frame.cellBuffer, frame.cellMemory, frame.cells);
    createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, frame.cellBuffer, frame.cellMemory);
    checkVk(vkMapMemory(device, frame.cellMemory, 0, bytes, 0, &frame.cells), "vkMapMemory");
    frame.cellCapacity = bytes;
    updateCellDescriptor(frame);
}

void RendererImpl::ensureFontUploadBuffer(FrameResources& frame, size_t bytes) {
    if (frame.fontUploadCapacity >= bytes) {
        return;
    }

    releaseBuffer(frame.fontUploadBuffer, frame.fontUploadMemory, frame.fontUploads);
    createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, frame.fontUploadBuffer, frame.fontUploadMemory);
    checkVk(vkMapMemory(device, frame.fontUploadMemory, 0, bytes, 0, &frame.fontUploads), "vkMapMemory");
    frame.fontUploadCapacity = bytes;
}

VkDeviceSize RendererImpl::stageFontData(const void* data, size_t len, size_t expected) {
    const u32 zero = 0;
    const size_t padding = (4 - (fontUploadData.used() & 3)) & 3;
    fontUploadData.append(&zero, padding);
    const VkDeviceSize offset = fontUploadData.used();
    if (len == expected) {
        fontUploadData.append(data, len);
    } else {
        fontUploadData.growDelta(expected);
        void* begin = fontUploadData.mutCurrent();
        fontUploadData.seekRelative(expected);
        memZero(begin, fontUploadData.mutCurrent());
    }
    return offset;
}

void RendererImpl::materializeCells(const TerminalCell* input, GpuCell* output, u16 count, u8 lineAttribute, const TerminalColors& colors) {
    CellExtraStore& extras = *composer.cellExtras;
    const bool specialColors = colors.specialModes != 0;
    for (u16 index = 0; index < count; ++index) {
        const TerminalCell& cell = input[index];
        const u32 codepoint = cell.uc_pt ? cell.uc_pt : ' ';
        const u32 attributes = cellAttributes(cell);
        const u32 foreground = specialColors ? colors.resolveForegroundSpecial(cell).packed() : colors.resolvePacked(cell.foreground());
        const u32 background = specialColors ? colors.resolveBackgroundSpecial(cell).packed() : colors.resolvePacked(cell.background());
        u32 underlineColor = foreground;
        u32 hyperlink = 0;
        if (cell.hasExtra()) {
            const CellExtraView extra = extras.view(cell);
            hyperlink = extra.hyperlinkDisplayId;
            if (cell.underlined() && extra.underlineColor != cell.foreground()) {
                underlineColor = colors.resolvePacked(extra.underlineColor);
            }
        } else if (cell.underlined() && cell.inlineUnderlineColor() != cell.foreground()) {
            underlineColor = colors.resolvePacked(cell.inlineUnderlineColor());
        }

        // The strip reference arrives in the row pass that follows the
        // span materialization; a cell it skips has none.
        output[index] = {
            codepoint,
            attributes,
            foreground,
            background,
            underlineColor,
            hyperlink,
            stripNone,
            0,
            cell.semantic,
            lineAttribute,
        };
    }
}

void RendererImpl::resetArenaStaging() {
    fontUploadData.reset();
    maskArenaCopies.clear();
    colorArenaCopies.clear();
}

void RendererImpl::ensureArenaBuffer(ArenaBuffer& arena, size_t bytes) {
    const VkDeviceSize needed = bytes < 4 ? 4 : bytes;
    if (arena.capacity >= needed) {
        return;
    }
    VkDeviceSize capacity = arena.capacity < 4096 ? 4096 : arena.capacity;
    while (capacity < needed) {
        capacity *= 2;
    }
    // Growth is rare (font or viewport change); a device drain keeps the
    // swap trivially safe against frames in flight.
    checkVk(vkDeviceWaitIdle(device), "vkDeviceWaitIdle");
    vkDestroyBuffer(device, arena.buffer, nullptr);
    vkFreeMemory(device, arena.memory, nullptr);
    arena.buffer = VK_NULL_HANDLE;
    arena.memory = VK_NULL_HANDLE;
    createBuffer(capacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, arena.buffer, arena.memory);
    arena.capacity = capacity;
    arena.uploaded = 0;
    updateStaticDescriptors();
}

void RendererImpl::stageArenaCopy(ArenaBuffer& arena, Vector<VkBufferCopy>& copies, const u8* data, size_t used) {
    if (used <= arena.uploaded) {
        return;
    }
    VkBufferCopy copy{};
    copy.srcOffset = stageFontData(data + arena.uploaded, used - arena.uploaded, used - arena.uploaded);
    copy.dstOffset = arena.uploaded;
    copy.size = used - arena.uploaded;
    copies.pushBack(copy);
    arena.uploaded = used;
}

void RendererImpl::stageArenaTails(Screen& shapes, u32 generation) {
    if (generation != stripGeneration) {
        // The strips moved wholesale (collection or font change): the
        // device copies restart from the beginning.
        fontResources->mask.uploaded = 0;
        fontResources->color.uploaded = 0;
    }
    const size_t maskUsed = shapes.spanMaskUsed();
    const size_t colorUsed = shapes.spanColorUsed() * sizeof(u32);
    ensureArenaBuffer(fontResources->mask, maskUsed);
    ensureArenaBuffer(fontResources->color, colorUsed);
    stageArenaCopy(fontResources->mask, maskArenaCopies, shapes.spanMask(), maskUsed);
    stageArenaCopy(fontResources->color, colorArenaCopies, (const u8*)(shapes.spanColor()), colorUsed);
}

void RendererImpl::applySpanStrips(u32 rowIndex, const ScreenRowSpan& span) {
    if (span.missing || span.end <= span.begin || span.end > cellColumns) {
        return;
    }
    const u32 stride = (u32)(span.end - span.begin) * composer.glyphWidth;
    for (u16 column = span.begin; column < span.end; ++column) {
        GpuCell& cell = cells.mut(rowIndex + column);
        cell.strip = (span.offset + (u32)(column - span.begin) * composer.glyphWidth) | (span.color ? stripColorPlane : 0);
        cell.stripStride = stride;
    }
}

void RendererImpl::assignRowStrips(Screen& shapes, u16 row) {
    const u32 rowIndex = (u32)(row)*cellColumns;
    GpuCell* const rowCells = cells.mutData() + rowIndex;
    for (u16 column = 0; column < cellColumns; ++column) {
        rowCells[column].strip = stripNone;
        rowCells[column].stripStride = 0;
    }
    const size_t count = shapes.rowSpans(row, spanScratch.mutData());
    for (size_t index = 0; index < count; ++index) {
        applySpanStrips(rowIndex, spanScratch[index]);
    }
}

void RendererImpl::overrideOverlayStrips(Screen& shapes, const TerminalUpdate& update) {
    if (update.overlayCount == 0) {
        return;
    }
    // The preedit preview covers the underlying strips wholesale: its
    // blank cells hide the text below them.
    const u32 rowIndex = (u32)(update.overlayRow) * cellColumns;
    for (u32 index = 0; index < update.overlayCount; ++index) {
        GpuCell& cell = cells.mut(rowIndex + update.overlayColumn + index);
        cell.strip = stripNone;
        cell.stripStride = 0;
    }
    const size_t count = shapes.shapeCells(update.overlayCells, update.overlayCount, update.overlayColumn, spanScratch.mutData());
    for (size_t index = 0; index < count; ++index) {
        applySpanStrips(rowIndex, spanScratch[index]);
    }
}

u32 RendererImpl::assignStrips(const TerminalUpdate& update, bool allRows) {
    Screen& shapes = *update.shapes;
    spanScratch.clear();
    spanScratch.grow(cellColumns);
    while (spanScratch.length() < cellColumns) {
        spanScratch.pushBack({});
    }
    // A shaping pass can collect the arenas and move every strip assigned
    // so far, so redo the walk until it closes within one generation.
    bool everything = allRows || shapes.spanGeneration() != stripGeneration;
    u32 generation;
    for (;;) {
        generation = shapes.spanGeneration();
        if (everything) {
            for (u16 row = 0; row < cellRows; ++row) {
                assignRowStrips(shapes, row);
            }
        } else {
            for (size_t index = 0; index < update.rowCount; ++index) {
                assignRowStrips(shapes, (u16)(update.rows[index].row));
            }
        }
        overrideOverlayStrips(shapes, update);
        if (generation == shapes.spanGeneration()) {
            return generation;
        }
        everything = true;
    }
}

void RendererImpl::recordArenaUploads(FrameResources& frame) {
    if (maskArenaCopies.empty() && colorArenaCopies.empty()) {
        return;
    }
    bufferBarrier(frame.commandBuffer, frame.fontUploadBuffer, fontUploadData.used(), VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const auto record = [&](const ArenaBuffer& arena, const Vector<VkBufferCopy>& copies) {
        if (copies.empty()) {
            return;
        }
        bufferBarrier(frame.commandBuffer, arena.buffer, arena.capacity, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyBuffer(frame.commandBuffer, frame.fontUploadBuffer, arena.buffer, copies.length(), copies.data());
        bufferBarrier(frame.commandBuffer, arena.buffer, arena.capacity, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    };
    record(fontResources->mask, maskArenaCopies);
    record(fontResources->color, colorArenaCopies);
}

u32 RendererImpl::packColor(const Color& color) {
    return (u32)(color.red) | ((u32)(color.green) << 8) | ((u32)(color.blue) << 16);
}

bool RendererImpl::sameSelection(const Rect& lhs, const Rect& rhs) {
    return lhs.tl == rhs.tl && lhs.br == rhs.br && lhs.rectangular == rhs.rectangular;
}

void RendererImpl::ensureDamageJournal(u32 rowCount) {
    if (damage.capacity >= rowCount) {
        return;
    }
    STD_ASSERT(damage.count == 0);
    damageJournalStorage.grow((size_t)(rowCount) * sizeof(RenderDamage::Entry));
    damage.configure((RenderDamage::Entry*)(damageJournalStorage.mutData()), rowCount);
}

void RendererImpl::fullDamage() {
    damage.full();
}

void RendererImpl::appendDamage(u32 firstRow, u32 rowCount) {
    STD_ASSERT((size_t)(firstRow) + rowCount <= cellRows);
    damage.add(firstRow, rowCount);
}

void RendererImpl::collectDamage() {
    u64 applied = damage.generation;
    if (chain->direct) {
        for (u32 index = 0; index < chain->initialized.length(); ++index) {
            if (chain->initialized[index] && chain->generations[index] < applied) {
                applied = chain->generations[index];
            }
        }
    } else if (chain->outputInitialized) {
        applied = chain->outputGeneration;
    }
    damage.collect(applied);
}

void RendererImpl::capturePresentationState(const TerminalUpdate& update) {
    presentationState.cursor = update.cursor;
    presentationState.selection = update.snappedSelection;
    presentationState.selectionForeground = update.selectionForeground;
    presentationState.selectionBackground = update.selectionBackground;
    presentationState.selectionColorMask = update.selectionColorMask;
    presentationState.hoveredHyperlink = update.hoveredHyperlink;
    presentationState.hoveredLinkBegin = update.hoveredLinkBegin;
    presentationState.hoveredLinkEnd = update.hoveredLinkEnd;
    presentationState.screenReverse = update.screenReverse;
    presentationState.blinkVisible = update.blinkVisible;
    presentationState.cursorBlink = update.cursorBlink;
}

u32 RendererImpl::materializeUpdates(FrameResources& frame, u64 appliedGeneration, bool initialized) {
    const size_t cellCount = cells.length();
    ensureCellBuffer(frame, cellCount * sizeof(GpuCellUpdate));
    auto* const gpuUpdates = (GpuCellUpdate*)(frame.cells);

    if (updateEpochs.used() < (size_t)(cellRows) * sizeof(u32)) {
        updateEpochs.zero((size_t)(cellRows) * sizeof(u32));
    }
    if (++updateEpoch == 0) {
        updateEpochs.zero(updateEpochs.used());
        updateEpoch = 1;
    }
    auto* const epochs = (u32*)(updateEpochs.mutData());
    u32 gpuUpdateCount = 0;

    const auto appendRow = [&](u32 row) {
        if (epochs[row] == updateEpoch) {
            return;
        }
        epochs[row] = updateEpoch;
        const u32 rowIndex = row * cellColumns;
        const u8 lineAttribute = (u8)(cells[rowIndex].lineAttribute);
        for (u32 column = 0; column < cellColumns; ++column) {
            const u32 sourceIndex = rowIndex + column;
            GpuCell cell = cells[sourceIndex];
            if (lineAttribute == 0) {
                if ((cell.attributes & gpuDoubleWidth) != 0 && (column + 1 >= cellColumns || (cells[sourceIndex + 1].attributes & gpuDoubleWidthContinuation) == 0)) {
                    cell.attributes &= ~gpuDoubleWidth;
                }
                STD_ASSERT(gpuUpdateCount < cellCount);
                gpuUpdates[gpuUpdateCount++] = {sourceIndex, sourceIndex, cell};
                continue;
            }
            // A double-size line draws each source cell into two output
            // cells; the right half of the row has no output.
            const u32 firstColumn = column * 2;
            if (firstColumn >= cellColumns) {
                break;
            }
            STD_ASSERT(gpuUpdateCount < cellCount);
            gpuUpdates[gpuUpdateCount++] = {sourceIndex, rowIndex + firstColumn, cell};
            if (firstColumn + 1 < cellColumns) {
                STD_ASSERT(gpuUpdateCount < cellCount);
                gpuUpdates[gpuUpdateCount++] = {sourceIndex, rowIndex + firstColumn + 1, cell};
            }
        }
    };

    if (damage.requiresFull(appliedGeneration, initialized)) {
        for (u32 row = 0; row < cellRows; ++row) {
            appendRow(row);
        }
    } else {
        for (u32 entryIndex = 0; entryIndex < damage.count; ++entryIndex) {
            const RenderDamage::Entry& entry = damage.entry(entryIndex);
            if (entry.generation <= appliedGeneration) {
                continue;
            }
            for (u32 row = 0; row < entry.count; ++row) {
                appendRow(entry.begin + row);
            }
        }
    }

    if (!fontUploadData.empty()) {
        ensureFontUploadBuffer(frame, fontUploadData.used());
        __builtin_memcpy(frame.fontUploads, fontUploadData.data(), fontUploadData.used());
    }
    return gpuUpdateCount;
}

void RendererImpl::recordCommands(FrameResources& frame, u32 imageIndex, const PresentationState& state, u32 updateCount, bool clearOutput) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVk(vkBeginCommandBuffer(frame.commandBuffer, &beginInfo), "vkBeginCommandBuffer");
    recordArenaUploads(frame);

    const VkImage output = chain->direct ? chain->images[imageIndex] : chain->output.image;
    const VkImageView outputView = chain->direct ? chain->views[imageIndex] : chain->output.view;
    const bool initialized = chain->direct ? chain->initialized[imageIndex] : chain->outputInitialized;
    updateOutputDescriptor(frame, outputView);

    // Between frames the blit-path output image rests in GENERAL, a
    // direct-path swapchain image in PRESENT_SRC.
    const VkImageLayout restingLayout = chain->direct ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_GENERAL;
    const VkAccessFlags restingAccess = chain->direct ? VK_ACCESS_MEMORY_READ_BIT : VK_ACCESS_TRANSFER_READ_BIT;
    if (clearOutput) {
        imageBarrier(frame.commandBuffer, output, 1, initialized ? restingAccess : 0, VK_ACCESS_TRANSFER_WRITE_BIT, initialized ? restingLayout : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, initialized ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkClearColorValue clearColor{{
            clearBackground.red / 255.0f,
            clearBackground.green / 255.0f,
            clearBackground.blue / 255.0f,
            1.0f,
        }};
        const VkImageSubresourceRange outputRange = imageRange(1);
        vkCmdClearColorImage(frame.commandBuffer, output, VK_IMAGE_LAYOUT_GENERAL, &clearColor, 1, &outputRange);

        imageBarrier(frame.commandBuffer, output, 1, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    } else {
        imageBarrier(frame.commandBuffer, output, 1, restingAccess, VK_ACCESS_SHADER_WRITE_BIT, restingLayout, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }

    if (updateCount != 0) {
        bufferBarrier(frame.commandBuffer, frame.cellBuffer, (size_t)(updateCount) * sizeof(GpuCellUpdate), VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        const PushConstants pushConstants{
            composer.glyphWidth,
            composer.glyphHeight,
            composer.columns,
            composer.rows,
            chain->direct ? chain->extent.width : composer.pixelWidth,
            chain->direct ? chain->extent.height : composer.pixelHeight,
            opts.border,
            packColor(state.cursor.color),
            state.cursor.posX,
            state.cursor.posY,
            (u32)(state.cursor.style),
            state.screenReverse ? 1u : 0u,
            state.selection.tl.x,
            state.selection.tl.y,
            state.selection.br.x,
            state.selection.br.y,
            state.selection.rectangular ? 1u : 0u,
            opts.showWraps ? 1u : 0u,
            packColor(state.selectionForeground),
            packColor(state.selectionBackground),
            state.selectionColorMask,
            state.blinkVisible ? 1u : 0u,
            state.cursorBlink ? 1u : 0u,
            state.hoveredHyperlink,
            state.hoveredLinkBegin,
            state.hoveredLinkEnd,
            updateCount,
        };
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &frame.descriptorSet, 0, nullptr);
        vkCmdPushConstants(frame.commandBuffer, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushConstants), &pushConstants);
        vkCmdDispatch(frame.commandBuffer, (updateCount + 63) / 64, 1, 1);
    }

    if (chain->direct) {
        imageBarrier(frame.commandBuffer, output, 1, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        checkVk(vkEndCommandBuffer(frame.commandBuffer), "vkEndCommandBuffer");
        return;
    }

    recordBlit(frame, imageIndex, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    checkVk(vkEndCommandBuffer(frame.commandBuffer), "vkEndCommandBuffer");
}

void RendererImpl::recordBlit(FrameResources& frame, u32 imageIndex, VkAccessFlags outputSrcAccess, VkPipelineStageFlags outputSrcStage) {
    imageBarrier(frame.commandBuffer, chain->output.image, 1, outputSrcAccess, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, outputSrcStage, VK_PIPELINE_STAGE_TRANSFER_BIT);

    const bool initialized = chain->initialized[imageIndex];
    imageBarrier(frame.commandBuffer, chain->images[imageIndex], 1, initialized ? VK_ACCESS_MEMORY_READ_BIT : 0, VK_ACCESS_TRANSFER_WRITE_BIT, initialized ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, initialized ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImageBlit blit{};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[1] = {(i32)(renderExtent.width), (i32)(renderExtent.height), 1};
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[1] = {(i32)(chain->extent.width), (i32)(chain->extent.height), 1};
    vkCmdBlitImage(frame.commandBuffer, chain->output.image, VK_IMAGE_LAYOUT_GENERAL, chain->images[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);

    imageBarrier(frame.commandBuffer, chain->images[imageIndex], 1, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

void RendererImpl::recordRepaintCommands(FrameResources& frame, u32 imageIndex) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVk(vkBeginCommandBuffer(frame.commandBuffer, &beginInfo), "vkBeginCommandBuffer");
    recordBlit(frame, imageIndex, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    checkVk(vkEndCommandBuffer(frame.commandBuffer), "vkEndCommandBuffer");
}

bool RendererImpl::acquirePresentFrame(u32 width, u32 height, FrameResources*& frame, u32& imageIndex, bool& recreateAfterPresent) {
    frame = &frames[currentFrame];
    checkVk(vkWaitForFences(device, 1, &frame->fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    VkResult result = vkAcquireNextImageKHR(device, chain->swapchain, UINT64_MAX, frame->imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (result == VK_ERROR_SURFACE_LOST_KHR) {
        throw SurfaceLost{};
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        tryCreateSwapchain(width, height);
        return false;
    }
    recreateAfterPresent = result == VK_SUBOPTIMAL_KHR;
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        failVk("vkAcquireNextImageKHR", result);
    }
    checkVk(vkResetCommandBuffer(frame->commandBuffer, 0), "vkResetCommandBuffer");
    return true;
}

bool RendererImpl::submitPresentFrame(u32 width, u32 height, FrameResources& frame, u32 imageIndex, bool recreateAfterPresent) {
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &frame.imageAvailable;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &frame.commandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &chain->semaphores[imageIndex];
    // Reset only when committed to submitting: a throw between an early
    // reset and the submit would leave the fence unsignaled forever, and
    // the next wait on it would hang.
    checkVk(vkResetFences(device, 1, &frame.fence), "vkResetFences");
    checkVk(vkQueueSubmit(queue, 1, &submitInfo, frame.fence), "vkQueueSubmit");
    chain->initialized.mut(imageIndex) = true;

    SwapchainPresentFenceInfo presentFenceInfo{};
    VkFence presentFence = VK_NULL_HANDLE;
    if (swapchainMaintenanceExtension != nullptr) {
        presentFence = chain->presentFences[imageIndex];
        if (chain->presentFencePending[imageIndex]) {
            checkVk(vkWaitForFences(device, 1, &presentFence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
            checkVk(vkResetFences(device, 1, &presentFence), "vkResetFences");
        }
        presentFenceInfo.sType = swapchainPresentFenceInfoType;
        presentFenceInfo.swapchainCount = 1;
        presentFenceInfo.pFences = &presentFence;
    }
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.pNext = swapchainMaintenanceExtension == nullptr ? nullptr : &presentFenceInfo;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &chain->semaphores[imageIndex];
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &chain->swapchain;
    presentInfo.pImageIndices = &imageIndex;
    VkResult result = vkQueuePresentKHR(queue, &presentInfo);
    if (result == VK_ERROR_SURFACE_LOST_KHR) {
        throw SurfaceLost{};
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR && result != VK_ERROR_OUT_OF_DATE_KHR) {
        failVk("vkQueuePresentKHR", result);
    }
    if (swapchainMaintenanceExtension != nullptr) {
        chain->presentFencePending.mut(imageIndex) = true;
    }

    const bool presented = result != VK_ERROR_OUT_OF_DATE_KHR;
    if (presented && !retiredSwapchains.empty()) {
        for (SwapchainResources* const retired : retiredSwapchains) {
            if (retired->gracePresents != 0) {
                --retired->gracePresents;
            }
        }
        collectRetiredSwapchains();
    }
    currentFrame = (currentFrame + 1) % framesInFlight;
    if (recreateAfterPresent || result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR) {
        tryCreateSwapchain(width, height);
    }
    collectRetiredSwapchains();
    return presented;
}

bool RendererImpl::repaint() {
    try {
        return repaintFrame();
    } catch (const SurfaceLost&) {
        // The surface died and this renderer can never present again.
        // Null the pointer so any stale caller crashes loudly; the object
        // itself stays alive in composer.rendererPool until frame()
        // replaces the pool — destruction happens there, safely off this
        // stack.
        composer.renderer = nullptr;
        return false;
    }
}

void RendererImpl::recordFrame(FrameResources& frame, u32 imageIndex) {
    const bool initialized = chain->direct ? chain->initialized[imageIndex] : chain->outputInitialized;
    const u64 appliedGeneration = chain->direct ? chain->generations[imageIndex] : chain->outputGeneration;
    const u32 updateCount = materializeUpdates(frame, appliedGeneration, initialized);
    const bool clearOutput = !initialized || appliedGeneration < clearDamageGeneration;
    recordCommands(frame, imageIndex, presentationState, updateCount, clearOutput);
    if (chain->direct) {
        chain->generations.mut(imageIndex) = damage.generation;
    } else {
        chain->outputGeneration = damage.generation;
        chain->outputInitialized = true;
    }
}

bool RendererImpl::repaintFrame() {
    const u32 width = renderExtent.width;
    const u32 height = renderExtent.height;
    if (!previousStateValid || cells.empty() || width == 0 || height == 0) {
        return false;
    }
    if (chain->swapchain == VK_NULL_HANDLE && !tryCreateSwapchain(width, height)) {
        return false;
    }
    if (chain->swapchain == VK_NULL_HANDLE) {
        return false;
    }

    FrameResources* frame = nullptr;
    u32 imageIndex = 0;
    bool recreateAfterPresent = false;
    if (!acquirePresentFrame(width, height, frame, imageIndex, recreateAfterPresent)) {
        return false;
    }

    if (!chain->direct && chain->outputInitialized) {
        recordRepaintCommands(*frame, imageIndex);
    } else {
        // Nothing new to upload on a repaint: the arenas on the device
        // are current, only the output image is stale.
        resetArenaStaging();
        recordFrame(*frame, imageIndex);
    }
    const bool presented = submitPresentFrame(width, height, *frame, imageIndex, recreateAfterPresent);
    collectDamage();
    return presented;
}

bool RendererImpl::present(const TerminalUpdate& update) {
    const u32 width = composer.pixelWidth;
    const u32 height = composer.pixelHeight;
    const size_t cellCount = (size_t)(composer.columns) * composer.rows;
    if (cellCount == 0 || width == 0 || height == 0) {
        return false;
    }

    const bool wrongExtent = renderExtent.width != width || renderExtent.height != height;
    if ((chain->swapchain == VK_NULL_HANDLE || wrongExtent) && !tryCreateSwapchain(width, height)) {
        return false;
    }
    if (chain->swapchain == VK_NULL_HANDLE) {
        return false;
    }
    if (update.colors == nullptr) {
        return false;
    }

    const bool shapeChanged = cellColumns != composer.columns || cellRows != composer.rows;
    if (shapeChanged) {
        // A reshaped grid needs every row before the retained cells mean
        // anything.
        if (update.rowCount != composer.rows) {
            return false;
        }
        for (size_t index = 0; index < update.rowCount; ++index) {
            if (update.rows[index].cells == nullptr || update.rows[index].row != index) {
                return false;
            }
        }
    }

    FrameResources* frame = nullptr;
    u32 imageIndex = 0;
    bool recreateAfterPresent = false;
    if (!acquirePresentFrame(width, height, frame, imageIndex, recreateAfterPresent)) {
        return false;
    }

    resetArenaStaging();
    if (shapeChanged) {
        damage.begin = 0;
        damage.count = 0;
        ensureDamageJournal(composer.rows);
        cells.clear();
        cells.grow(cellCount);
        const GpuCell empty;
        for (size_t index = 0; index < cellCount; ++index) {
            cells.pushBack(empty);
        }
        cellColumns = composer.columns;
        cellRows = composer.rows;
    } else {
        ensureDamageJournal(cellRows);
    }
    for (size_t index = 0; index < update.rowCount; ++index) {
        const TerminalRow& row = update.rows[index];
        STD_ASSERT(row.cells != nullptr);
        STD_ASSERT(row.row < cellRows);
        materializeCells(row.cells, cells.mutData() + (size_t)(row.row) * cellColumns, cellColumns, row.lineAttribute, *update.colors);
    }
    const bool overlayValid = update.overlayCount != 0 && update.overlayCells != nullptr && update.overlayRow < cellRows && (size_t)(update.overlayColumn) + update.overlayCount <= cellColumns;
    if (overlayValid) {
        // The preview covers the row content beneath it.
        materializeCells(update.overlayCells, cells.mutData() + (size_t)(update.overlayRow) * cellColumns + update.overlayColumn, update.overlayCount, 0, *update.colors);
    }
    u32 arenaGeneration = stripGeneration;
    if (update.shapes != nullptr) {
        arenaGeneration = assignStrips(update, shapeChanged || !previousStateValid);
        stageArenaTails(*update.shapes, arenaGeneration);
    }
    const bool stripsMoved = arenaGeneration != stripGeneration;
    stripGeneration = arenaGeneration;

    if (damage.advance()) {
        clearDamageGeneration = damage.generation;
        for (u32 index = 0; index < chain->generations.length(); ++index) {
            chain->generations.mut(index) = 0;
        }
        chain->outputGeneration = 0;
    }
    const bool selectionChanged = previousStateValid && (!sameSelection(update.snappedSelection, previousSelection) || previousHoveredLinkBegin != update.hoveredLinkBegin || previousHoveredLinkEnd != update.hoveredLinkEnd);
    const bool globalPresentationChanged = previousStateValid && (presentationState.screenReverse != update.screenReverse || !(presentationState.selectionForeground == update.selectionForeground) || !(presentationState.selectionBackground == update.selectionBackground) || presentationState.selectionColorMask != update.selectionColorMask);
    // The padding follows the live default background (OSC 11); a change
    // needs a full clear, not just cell repaints.
    const bool backgroundChanged = !(clearBackground == update.colors->defaultBackground);
    clearBackground = update.colors->defaultBackground;
    if (shapeChanged || !previousStateValid || globalPresentationChanged || backgroundChanged || stripsMoved) {
        fullDamage();
        if (shapeChanged || backgroundChanged) {
            clearDamageGeneration = damage.generation;
        }
    } else {
        if (selectionChanged) {
            // Repaint only the rows the old and new selections cover: a
            // drag must not repaint the whole grid every frame.
            const auto damageSelectionRows = [&](const Rect& selection) {
                const i32 firstRow = std::max(selection.tl.y, 0);
                const i32 lastRow = std::min<i32>(selection.br.y, (i32)(cellRows)-1);
                if (firstRow > lastRow) {
                    return;
                }
                appendDamage((u32)(firstRow), (u32)(lastRow - firstRow + 1));
            };
            damageSelectionRows(previousSelection);
            damageSelectionRows(update.snappedSelection);
            const auto damageLinkSpan = [&](u32 begin, u32 end) {
                if (begin < end && end <= cellCount) {
                    const u32 firstRow = begin / cellColumns;
                    const u32 lastRow = (end - 1) / cellColumns;
                    appendDamage(firstRow, lastRow - firstRow + 1);
                }
            };
            damageLinkSpan(previousHoveredLinkBegin, previousHoveredLinkEnd);
            damageLinkSpan(update.hoveredLinkBegin, update.hoveredLinkEnd);
        }
        for (size_t index = 0; index < update.rowCount; ++index) {
            appendDamage(update.rows[index].row, 1);
        }
        if (overlayValid) {
            appendDamage(update.overlayRow, 1);
        }

        const auto appendCursor = [&](const TerminalCursor& cursor) {
            if (cursor.posX >= 0 && cursor.posY >= 0 && cursor.posX < cellColumns && cursor.posY < cellRows) {
                appendDamage((u32)(cursor.posY), 1);
            }
        };
        appendCursor(previousCursor);
        appendCursor(update.cursor);

        if (previousHoveredHyperlink != update.hoveredHyperlink) {
            for (u32 index = 0; index < cellCount; ++index) {
                const u32 hyperlink = cells[index].hyperlink;
                if (hyperlink == previousHoveredHyperlink || hyperlink == update.hoveredHyperlink) {
                    appendDamage(index / cellColumns, 1);
                }
            }
        }
        if (presentationState.blinkVisible != update.blinkVisible) {
            for (u32 index = 0; index < cellCount; ++index) {
                if ((cells[index].attributes & gpuBlink) != 0) {
                    appendDamage(index / cellColumns, 1);
                }
            }
        }
    }
    capturePresentationState(update);

    recordFrame(*frame, imageIndex);
    previousCursor = update.cursor;
    previousSelection = update.snappedSelection;
    previousHoveredHyperlink = update.hoveredHyperlink;
    previousHoveredLinkBegin = update.hoveredLinkBegin;
    previousHoveredLinkEnd = update.hoveredLinkEnd;
    previousStateValid = true;
    const bool presented = submitPresentFrame(width, height, *frame, imageIndex, recreateAfterPresent);
    collectDamage();
    return presented;
}

bool RendererImpl::update(const TerminalUpdate& update) {
    try {
        return present(update);
    } catch (const SurfaceLost&) {
        // See repaint(): mark dead, frame() rebuilds pool and renderer.
        composer.renderer = nullptr;
        return false;
    }
}

Renderer* createVulkanRenderer(Composer& composer, stl::ObjPool& pool, const plt::RenderContext& context) {
    RendererImpl* const renderer = pool.make<RendererImpl>(composer, context);
    composer.fontChangedListeners.pushBack(pool.make<CallRendererFontChanged>(renderer));
    return renderer;
}
