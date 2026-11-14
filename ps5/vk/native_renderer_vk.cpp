// reColossus: Celosia's native renderer on Vulkan (the PS5's RADV, linked into the app), a port of the OpenGL backend
// (PS2Recomp/ps2xRuntime/src/lib/native/native_renderer_gl.cpp) pass for pass, so that the game's models are drawn
// by the GPU and neither the VU1 nor the GS is emulated for them.
//
// The pictures keep the GL backend's layout in memory: clip y = -1 is row 0 (GL's bottom row, Vulkan's top), the
// textures' v = 0 is row 0 in both, and gl_FragCoord counts rows from row 0 in both, so every shader is the GL one
// (ps5/vk/, compiled by tools/spirv.sh) and the picture is turned over once when it is read back, as the GL backend
// does. Depth is the GL backend's 1 - 0.1 / w (Vulkan's clip z = w - 0.1).
//
// Every GL state change becomes a pipeline (blend, depth test and write, stencil: made when first drawn with, kept),
// every glClear inside a target a vkCmdClearAttachments, and every texture bound a descriptor. Render passes load
// and store their attachments; a target is turned into what the next use needs (drawn into, read by a shader,
// copied) with a barrier between render passes, which also orders one pass's writes before the next one's.
//
// Version 1 reads each picture back (the host's raylib subset shows it, as it shows the emulated GS's), and draws
// on a render thread of its own like the GL backend (the game thread's side, GlBackend there, is VkBackend here,
// the same code): the thread records the frame, submits it and waits for it, then reads it back.
// The Vulkan entry points: RADV's vk_icdGetInstanceProcAddr on the PS5 (no loader there, as in reKing), the
// system's libvulkan elsewhere (tests/vk_check.cpp runs this file on Linux against lavapipe and the GL backend).
#include "native_backend.h"
#include "runtime/native_renderer.h"
#include "runtime/host_menu.h"
#include "runtime/native_tuning.h"
#include "ThreadNaming.h"

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

#include "native_vk_spirv.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(PLATFORM_PS5) || defined(__PROSPERO__)
#define RC_VK_STATIC_ICD 1
extern "C" PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);
#include "ps5/rc_screen.h"
#else
#define RC_VK_STATIC_ICD 0
#include <dlfcn.h>
#include <malloc.h>
#endif

namespace
{
    uint64_t heapUsed()
    {
#if !RC_VK_STATIC_ICD && defined(__linux__)
        return mallinfo2().uordblks;
#else
        return 0; // the memory log only (no heap limit off the Vita)
#endif
    }

    // ---- Entry points ----
#define RC_VK_INSTANCE_FUNCS(X)                                                                                         \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceFeatures)  \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties)                                  \
    X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define RC_VK_DEVICE_FUNCS(X)                                                                                           \
    X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory)                          \
    X(vkInvalidateMappedMemoryRanges) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements)             \
    X(vkBindBufferMemory) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory)       \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkCreateShaderModule)                               \
    X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) X(vkCreateRenderPass)         \
    X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateDescriptorPool) X(vkResetDescriptorPool)                   \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateCommandPool) X(vkResetCommandPool)                  \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkCreateFence)         \
    X(vkWaitForFences) X(vkResetFences) X(vkCmdPipelineBarrier) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass)           \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdDraw)      \
    X(vkCmdDrawIndexed) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdPushConstants) X(vkCmdClearAttachments)           \
    X(vkCmdClearColorImage) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer)
    // The display (VK_KHR_display on the PS5's VideoOut, PS5_Mesa's wsi_common_videoout.c) and its swapchain:
    // loaded when there, the picture read back to the host otherwise.
#define RC_VK_DISPLAY_INSTANCE_FUNCS(X)                                                                                 \
    X(vkEnumerateDeviceExtensionProperties) X(vkGetPhysicalDeviceDisplayPropertiesKHR) X(vkGetDisplayModePropertiesKHR)  \
    X(vkCreateDisplayPlaneSurfaceKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)
#define RC_VK_DISPLAY_DEVICE_FUNCS(X)                                                                                   \
    X(vkCreateSwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkCreateSemaphore)
#define RC_VK_DECLARE(name) PFN_##name name = nullptr;
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddrFn = nullptr;
    PFN_vkCreateInstance vkCreateInstance = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties = nullptr;
    RC_VK_INSTANCE_FUNCS(RC_VK_DECLARE)
    RC_VK_DEVICE_FUNCS(RC_VK_DECLARE)
    RC_VK_DISPLAY_INSTANCE_FUNCS(RC_VK_DECLARE)
    RC_VK_DISPLAY_DEVICE_FUNCS(RC_VK_DECLARE)
#undef RC_VK_DECLARE

    const char *resultName(VkResult r)
    {
        switch (r)
        {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
        case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
        default: return "VkResult";
        }
    }
    bool check(VkResult r, const char *what)
    {
        if (r == VK_SUCCESS)
            return true;
        static int reported = 0;
        if (reported++ < 40)
            std::fprintf(stderr, "[native:vk] %s: %s (%d)\n", what, resultName(r), static_cast<int>(r));
        return false;
    }

    // ---- Device memory: blocks of 64 MB, ranges handed out first fit ----
    struct Alloc
    {
        uint32_t pool = ~0u, block = 0;
        VkDeviceSize offset = 0, size = 0;
        explicit operator bool() const { return pool != ~0u; }
    };
    struct MemBlock
    {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkBuffer buffer = VK_NULL_HANDLE; // buffer pools: one buffer over the whole block
        uint8_t *mapped = nullptr;
        VkDeviceSize size = 0;
        std::map<VkDeviceSize, VkDeviceSize> free; // offset -> size
    };
    struct MemPool
    {
        uint32_t type = 0;
        bool buffers = false;
        VkBufferUsageFlags usage = 0;
        std::vector<MemBlock> blocks;
    };

    // A buffer of its own (the frame's streams, staging, the read-back), mapped.
    struct Stream
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint8_t *mapped = nullptr;
        VkDeviceSize size = 0;
        bool coherent = true;
    };

    struct Image
    {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;   // sampled (depth images: the depth aspect), colour attachment
        VkImageView attach = VK_NULL_HANDLE; // depth images: depth and stencil, as the attachment
        Alloc memory;
        uint32_t width = 0, height = 0, levels = 1;
        bool depth = false;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    struct Mesh
    {
        Alloc vertices, indices; // kept meshes; per-frame ones are ranges of the frame's streams
        uint32_t count = 0;
        bool dynamic = false; // in this frame's arena, from vertex `first`
        uint32_t first = 0;
        bool indexed = false; // `count` indices: dynamic from indexArena[firstIndex], else in `indices`
        uint32_t firstIndex = 0;
        bool skinned = false;
        uint64_t frame = 0, used = 0, bytes = 0;
    };

    struct Texture
    {
        Image image;
        uint64_t used = 0, bytes = 0;
    };

    struct Draw
    {
        Draw(uint64_t m, const float c[16], const native_renderer::DrawState &s) : mesh(m), state(s) { std::memcpy(clip, c, sizeof(clip)); }
        uint64_t mesh;
        native_renderer::DrawState state;
        float clip[16];
    };

    // A colour target, with its depth / stencil when it has one (the GL backend's Target).
    struct Target
    {
        Image color, depth;
        uint32_t width = 0, height = 0;
    };
    // What a pass draws into: a colour image and maybe a depth one (another target's: the shadow mask and the glow
    // layer use the picture's).
    struct Fb
    {
        Image *color = nullptr;
        Image *depth = nullptr;
    };

    // The main shaders' per-draw values (std140 vec4s: main.vert / main.frag's Draw block).
    struct DrawUniforms
    {
        float clip[16], directions[16], colours[16];
        float misc[4], fogA[4], fogB[4], fogC[4], fogColour[4], debug[4];
    };
    // The full-screen passes' (quad.vert / quad.frag's Push block).
    struct QuadPush
    {
        float a[4], b[4], quad[4];
        int32_t mode, corners;
    };

    struct Renderer
    {
        // ---- the device ----
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice gpu = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        uint32_t queueFamily = 0;
        VkPhysicalDeviceProperties props{};
        VkPhysicalDeviceMemoryProperties memProps{};
        bool depthClamp = false, anisotropy = false;
        float maxAnisotropy = 1.0f;
        VkFormat depthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool recording = false, lost = false;

        VkRenderPass rpColor = VK_NULL_HANDLE, rpColorDepth = VK_NULL_HANDLE;
        VkDescriptorSetLayout frameLayout = VK_NULL_HANDLE, textureLayout = VK_NULL_HANDLE, quadSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout mainLayout = VK_NULL_HANDLE, quadLayout = VK_NULL_HANDLE;
        VkShaderModule mainVert = VK_NULL_HANDLE, skinVert = VK_NULL_HANDLE, mainFrag = VK_NULL_HANDLE, quadVert = VK_NULL_HANDLE,
                       quadFrag = VK_NULL_HANDLE;
        bool skinOk = false;
        // Pipelines as they are first needed. Main: skinned, blend, depth test, depth write, shadow volume.
        VkPipeline mainPipelines[64] = {};
        // Full-screen passes: with a depth attachment, blend, stencil (the shadows' count tested).
        VkPipeline quadPipelines[2][4][2] = {};
        VkSampler gameSamplers[64] = {}; // clamp s, clamp t, filter (0..6), mipped
        VkSampler linearClamp = VK_NULL_HANDLE, nearestClamp = VK_NULL_HANDLE;
        Image dummy; // 1 x 1 black: what a shader's unused texture reads
        std::map<std::pair<VkImageView, VkImageView>, VkFramebuffer> framebuffers;

        std::vector<MemPool> pools;
        uint32_t meshPool = ~0u;
        std::map<uint32_t, uint32_t> imagePools; // memory type -> pool
        Stream vertexStream, indexStream, paletteStream, staging, readback;
        std::vector<Stream> uniformChunks;
        VkDeviceSize uniformStride = 0;
        static constexpr VkDeviceSize kUniformChunk = 1u << 20;
        size_t uniformChunk = 0;
        VkDeviceSize uniformUsed = 0;
        VkDescriptorSet frameSet = VK_NULL_HANDLE; // this chunk's
        std::vector<VkDescriptorPool> descriptorPools;
        size_t descriptorPool = 0;

        // ---- recording state ----
        bool inPass = false;
        VkImage passColor = VK_NULL_HANDLE, passDepth = VK_NULL_HANDLE;
        VkPipeline boundPipeline = VK_NULL_HANDLE;
        VkBuffer boundVertexBuffer = VK_NULL_HANDLE, boundIndexBuffer = VK_NULL_HANDLE;
        VkDeviceSize boundVertexOffset = ~0ull, boundIndexOffset = ~0ull;
        struct Binding
        {
            VkImageView view = VK_NULL_HANDLE;
            VkSampler sampler = VK_NULL_HANDLE;
            VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
            bool operator==(const Binding &o) const { return view == o.view && sampler == o.sampler && layout == o.layout; }
        };
        Binding lastTextures[4];
        VkDescriptorSet lastTextureSet = VK_NULL_HANDLE;
        Binding lastQuad[3];
        VkDescriptorSet lastQuadSet = VK_NULL_HANDLE;
        Image *unit[3] = {}; // the full-screen passes' textures (the GL backend's units 0..2): uSrc, uSrc2, uDepth

        // ---- the GL backend's state, as it is there ----
        Target color, background, backgroundSmall, reflection, refraction, refractionHalf, snapshot, shadowMask, previous;
        Target prefix[3], hazeSmall;
        Target glowLayer, glowSmall[4];
        Target bloomTex[3];
        bool bloomHistory = false;
        Target gsBloom[7];
        bool gsBloomHistory = false;
        Fb colorPlain, shadowMaskFb, glowLayerFb;
        uint32_t width = 0, height = 0;
        std::unordered_map<uint64_t, Mesh> meshes;
        std::unordered_map<uint64_t, Texture> textures;
        uint64_t textureBytes = 0, textureHeapBytes = 0, meshBytes = 0, skinnedBytes = 0;
        void forget(const Mesh &m)
        {
            meshBytes -= m.bytes;
            if (m.skinned)
                skinnedBytes -= m.bytes;
        }
        void forget(const Texture &t) { textureBytes -= t.bytes; }
        std::vector<Draw> draws;
        std::vector<uint64_t> drawOrder;
        std::vector<float> palettes;
        template <typename T>
        struct Arena
        {
            std::unique_ptr<T[]> items;
            size_t count = 0, capacity = 0;
            bool empty() const { return count == 0; }
            size_t size() const { return count; }
            const T *data() const { return items.get(); }
            void clear() { count = 0; }
            size_t bytes() const { return capacity * sizeof(T); }
            T *grow(size_t n)
            {
                if (count + n > capacity)
                {
                    const size_t want = std::max(count + n, std::min(capacity * 2u + 4096u, count + n + (1u << 20)));
                    std::unique_ptr<T[]> bigger(new (std::nothrow) T[want]);
                    if (!bigger)
                        return nullptr;
                    if (count)
                        std::memcpy(bigger.get(), items.get(), count * sizeof(T));
                    items = std::move(bigger);
                    capacity = want;
                }
                T *at = items.get() + count;
                count += n;
                return at;
            }
            bool append(const T *from, size_t n)
            {
                if (!n)
                    return true;
                T *at = grow(n);
                if (at)
                    std::memcpy(at, from, n * sizeof(T));
                return at != nullptr;
            }
            void trim(size_t keep)
            {
                if (count || capacity <= keep)
                    return;
                std::unique_ptr<T[]> smaller(keep ? new (std::nothrow) T[keep] : nullptr);
                if (keep && !smaller)
                    return;
                items = std::move(smaller);
                capacity = keep;
            }
        };
        Arena<native_renderer::Vertex> arena;
        Arena<uint32_t> indexArena;
        uint64_t arenaFrame = 0;
        struct
        {
            uint64_t missing = 0, uploads = 0;
        } texStats;
        struct Upload
        {
            enum Kind : uint8_t
            {
                MeshKind,
                SkinnedKind,
                TextureKind
            } kind = MeshKind;
            uint64_t id = 0;
            uint32_t count = 0, width = 0, height = 0, levels = 1;
            std::vector<uint8_t> bytes;
            std::vector<uint32_t> indices;
        };
        struct FrameMesh
        {
            uint64_t id;
            uint32_t first, count, firstIndex;
            bool indexed;
        };
        std::vector<Upload> uploads;
        std::vector<FrameMesh> frameMeshes;
        std::vector<Mesh> deadMeshes;       // evicted (VkBackend::evictKept): freed before the next frame
        std::vector<Texture> deadTextures;
        struct
        {
            uint64_t renderUs = 0, presentUs = 0;
            uint32_t targets = 0;
        } stats;
        std::vector<native_renderer::ScreenBlur> blurs;
        native_renderer::Fog fog;
        native_renderer::Bloom bloom;
        native_renderer::GlowComposite glow;
        native_renderer::DepthOfField dof;
        bool hold = false;

        // ---- the display (direct): the picture fitted to it, the host's overlay over it ----
        bool displayExtensions = false, direct = false, headless = false;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkFormat swapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
        VkExtent2D screen{0, 0};
        std::vector<VkImage> swapchainImages;
        std::vector<VkImageView> swapchainViews;
        std::vector<VkFramebuffer> swapchainFramebuffers;
        VkRenderPass rpPresent = VK_NULL_HANDLE;
        VkShaderModule presentVert = VK_NULL_HANDLE, presentFrag = VK_NULL_HANDLE;
        VkPipeline presentPicture = VK_NULL_HANDLE, presentOverlay = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE, drawn = VK_NULL_HANDLE;
        Image overlay, cpuPicture;
        Stream overlayStaging, cpuStaging;
        unsigned long long cpuSerial = 0, overlaySeen = 0;
        int cpuWidth = 0, cpuHeight = 0;
        uint64_t lastPresentUs = 0, lastNativePresentUs = 0, lastIdleCheckUs = 0;
        bool presentCpu = false; // this present shows the host's picture (the emulated GS's), not the renderer's
        bool idleDue() const
        {
            const uint64_t now = native_renderer::nowUs();
            return direct && now - lastNativePresentUs >= 100000u && now - lastIdleCheckUs >= 8000u;
        }
        void presentIdle();
        void queuePresent();
        bool presentNext = false;     // this frame's commands end with the present pass
        uint32_t presentImage = 0;
        float pictureAspect = 4.0f / 3.0f; // the display aspect the picture is shown at (this frame's)
        float cpuAspect = 4.0f / 3.0f;     // the host's pictures' (the emulated GS's)
        bool pictureStretched = false;
        bool setupDisplay();
        bool setupSwapchain(VkExtent2D extent);
        void recordPresent();
        // The rectangle of the screen the picture takes: the aspect, centred (or all of it, stretched).
        VkRect2D fitRect() const
        {
            if (pictureStretched || !screen.width || !screen.height)
                return VkRect2D{{0, 0}, screen};
            const float sw = static_cast<float>(screen.width), sh = static_cast<float>(screen.height);
            const float w = std::min(sw, sh * pictureAspect), h = w / pictureAspect;
            const uint32_t iw = static_cast<uint32_t>(std::lround(w)) & ~1u, ih = static_cast<uint32_t>(std::lround(h)) & ~1u;
            return VkRect2D{{static_cast<int32_t>((screen.width - iw) / 2u), static_cast<int32_t>((screen.height - ih) / 2u)}, {iw, ih}};
        }

        std::mutex outputMutex;
        std::vector<uint8_t> output;
        uint32_t outputWidth = 0, outputHeight = 0;
        bool hasOutput = false;
        uint32_t readbackWidth = 0, readbackHeight = 0;

        // =============================== setup ===============================
        bool init();
        uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags need, VkMemoryPropertyFlags want = 0) const
        {
            uint32_t best = ~0u;
            for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
            {
                const VkMemoryPropertyFlags f = memProps.memoryTypes[i].propertyFlags;
                if (!(bits & (1u << i)) || (f & need) != need)
                    continue;
                if ((f & want) == want)
                    return i;
                if (best == ~0u)
                    best = i;
            }
            return best;
        }
        bool makeStream(Stream &s, VkDeviceSize size, VkBufferUsageFlags usage, bool cached = false)
        {
            destroyStream(s);
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = std::max<VkDeviceSize>(size, 256);
            bi.usage = usage;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (!check(vkCreateBuffer(device, &bi, nullptr, &s.buffer), "vkCreateBuffer (stream)"))
                return false;
            VkMemoryRequirements req{};
            vkGetBufferMemoryRequirements(device, s.buffer, &req);
            const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
            uint32_t type = cached ? memoryType(req.memoryTypeBits, host | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                   : memoryType(req.memoryTypeBits, host | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (type == ~0u)
                type = memoryType(req.memoryTypeBits, host | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (type == ~0u)
                return false;
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = type;
            if (!check(vkAllocateMemory(device, &ai, nullptr, &s.memory), "vkAllocateMemory (stream)"))
                return false;
            vkBindBufferMemory(device, s.buffer, s.memory, 0);
            void *p = nullptr;
            if (!check(vkMapMemory(device, s.memory, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory (stream)"))
                return false;
            s.mapped = static_cast<uint8_t *>(p);
            s.size = bi.size;
            s.coherent = (memProps.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
            return true;
        }
        void destroyStream(Stream &s)
        {
            if (s.buffer)
                vkDestroyBuffer(device, s.buffer, nullptr);
            if (s.memory)
                vkFreeMemory(device, s.memory, nullptr);
            s = Stream{};
        }
        // At least `size` (the GPU is idle whenever this is called: between submissions, or before this one's use).
        bool ensureStream(Stream &s, VkDeviceSize size, VkBufferUsageFlags usage, bool cached = false)
        {
            if (s.buffer && s.size >= size)
                return true;
            VkDeviceSize grown = std::max<VkDeviceSize>(size + size / 4u, 1u << 16);
            return makeStream(s, grown, usage, cached);
        }

        uint32_t makePool(uint32_t type, bool buffers, VkBufferUsageFlags usage)
        {
            MemPool p;
            p.type = type;
            p.buffers = buffers;
            p.usage = usage;
            pools.push_back(std::move(p));
            return static_cast<uint32_t>(pools.size() - 1u);
        }
        Alloc allocate(uint32_t poolIndex, VkDeviceSize size, VkDeviceSize align)
        {
            MemPool &pool = pools[poolIndex];
            align = std::max<VkDeviceSize>(align, 16);
            auto carve = [&](uint32_t b) -> Alloc
            {
                MemBlock &block = pool.blocks[b];
                for (auto it = block.free.begin(); it != block.free.end(); ++it)
                {
                    const VkDeviceSize start = (it->first + align - 1u) / align * align, end = it->first + it->second;
                    if (start + size > end)
                        continue;
                    const VkDeviceSize from = it->first;
                    block.free.erase(it);
                    if (start > from)
                        block.free.emplace(from, start - from);
                    if (start + size < end)
                        block.free.emplace(start + size, end - (start + size));
                    Alloc a;
                    a.pool = poolIndex;
                    a.block = b;
                    a.offset = start;
                    a.size = size;
                    return a;
                }
                return Alloc{};
            };
            for (uint32_t b = 0; b < pool.blocks.size(); ++b)
                if (pool.blocks[b].memory)
                    if (Alloc a = carve(b))
                        return a;
            // A new block (a freed one's slot when there is one).
            MemBlock block;
            block.size = std::max<VkDeviceSize>(64ull << 20, (size + align + 0xFFFFu) & ~VkDeviceSize(0xFFFFu));
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = block.size;
            ai.memoryTypeIndex = pool.type;
            if (pool.buffers)
            {
                VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                bi.size = block.size;
                bi.usage = pool.usage;
                bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                if (!check(vkCreateBuffer(device, &bi, nullptr, &block.buffer), "vkCreateBuffer (pool)"))
                    return Alloc{};
                VkMemoryRequirements req{};
                vkGetBufferMemoryRequirements(device, block.buffer, &req);
                ai.allocationSize = std::max(req.size, block.size);
            }
            if (!check(vkAllocateMemory(device, &ai, nullptr, &block.memory), "vkAllocateMemory (pool)"))
            {
                if (block.buffer)
                    vkDestroyBuffer(device, block.buffer, nullptr);
                return Alloc{};
            }
            if (pool.buffers)
            {
                vkBindBufferMemory(device, block.buffer, block.memory, 0);
                void *p = nullptr;
                if (check(vkMapMemory(device, block.memory, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory (pool)"))
                    block.mapped = static_cast<uint8_t *>(p);
            }
            block.free.emplace(0, block.size);
            uint32_t at = static_cast<uint32_t>(pool.blocks.size());
            for (uint32_t b = 0; b < pool.blocks.size(); ++b)
                if (!pool.blocks[b].memory)
                {
                    at = b;
                    break;
                }
            if (at == pool.blocks.size())
                pool.blocks.push_back(std::move(block));
            else
                pool.blocks[at] = std::move(block);
            return carve(at);
        }
        void release(Alloc &a)
        {
            if (!a)
                return;
            MemBlock &block = pools[a.pool].blocks[a.block];
            VkDeviceSize offset = a.offset, size = a.size;
            auto next = block.free.lower_bound(offset);
            if (next != block.free.end() && offset + size == next->first)
            {
                size += next->second;
                next = block.free.erase(next);
            }
            if (next != block.free.begin())
            {
                auto prev = std::prev(next);
                if (prev->first + prev->second == offset)
                {
                    offset = prev->first;
                    size += prev->second;
                    block.free.erase(prev);
                }
            }
            block.free.emplace(offset, size);
            a = Alloc{};
        }
        MemBlock &blockOf(const Alloc &a) { return pools[a.pool].blocks[a.block]; }

        bool makeImage(Image &img, uint32_t w, uint32_t h, uint32_t levels, bool depthImage, VkImageUsageFlags usage,
                       VkFormat format = VK_FORMAT_R8G8B8A8_UNORM)
        {
            destroyImage(img);
            VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ci.imageType = VK_IMAGE_TYPE_2D;
            ci.format = depthImage ? depthFormat : format;
            ci.extent = {std::max(w, 1u), std::max(h, 1u), 1};
            ci.mipLevels = std::max(levels, 1u);
            ci.arrayLayers = 1;
            ci.samples = VK_SAMPLE_COUNT_1_BIT;
            ci.tiling = VK_IMAGE_TILING_OPTIMAL;
            ci.usage = usage;
            ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (!check(vkCreateImage(device, &ci, nullptr, &img.image), "vkCreateImage"))
            {
                img.image = VK_NULL_HANDLE;
                return false;
            }
            VkMemoryRequirements req{};
            vkGetImageMemoryRequirements(device, img.image, &req);
            const uint32_t type = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != ~0u
                                      ? memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                                      : memoryType(req.memoryTypeBits, 0);
            auto found = imagePools.find(type);
            const uint32_t pool = found != imagePools.end() ? found->second : (imagePools[type] = makePool(type, false, 0));
            img.memory = allocate(pool, req.size, req.alignment);
            if (!img.memory)
            {
                vkDestroyImage(device, img.image, nullptr);
                img.image = VK_NULL_HANDLE;
                return false;
            }
            vkBindImageMemory(device, img.image, blockOf(img.memory).memory, img.memory.offset);
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = img.image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = ci.format;
            vi.subresourceRange = {static_cast<VkImageAspectFlags>(depthImage ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, ci.mipLevels, 0, 1};
            check(vkCreateImageView(device, &vi, nullptr, &img.view), "vkCreateImageView");
            if (depthImage)
            {
                vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
                check(vkCreateImageView(device, &vi, nullptr, &img.attach), "vkCreateImageView (depth)");
            }
            img.width = ci.extent.width;
            img.height = ci.extent.height;
            img.levels = ci.mipLevels;
            img.depth = depthImage;
            img.layout = VK_IMAGE_LAYOUT_UNDEFINED;
            return true;
        }
        void destroyImage(Image &img)
        {
            if (img.view)
                vkDestroyImageView(device, img.view, nullptr);
            if (img.attach)
                vkDestroyImageView(device, img.attach, nullptr);
            if (img.image)
                vkDestroyImage(device, img.image, nullptr);
            release(img.memory);
            img = Image{};
        }
        void forgetFramebuffers()
        {
            for (auto &f : framebuffers)
                vkDestroyFramebuffer(device, f.second, nullptr);
            framebuffers.clear();
            lastTextureSet = lastQuadSet = VK_NULL_HANDLE;
        }
        void makeTarget(Target &t, uint32_t w, uint32_t h, bool withDepth)
        {
            destroyImage(t.color);
            destroyImage(t.depth);
            t.width = w;
            t.height = h;
            makeImage(t.color, w, h, 1, false,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            if (withDepth)
                makeImage(t.depth, w, h, 1, true, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        }
        static Fb fb(Target &t) { return Fb{&t.color, t.depth.image ? &t.depth : nullptr}; }
        void ensureTargets(uint32_t w, uint32_t h)
        {
            if (color.color.image && width == w && height == h)
                return;
            forgetFramebuffers();
            makeTarget(color, w, h, true);
            makeTarget(background, w, h, true);
            makeTarget(snapshot, w, h, false);
            makeTarget(shadowMask, w, h, false);
            makeTarget(previous, w, h, false);
            makeTarget(glowLayer, w, h, false);
            for (int i = 0; i < 4; ++i)
                makeTarget(glowSmall[i], i == 0 ? 256u : (i == 1 ? 128u : 64u), i == 0 ? 256u : (i == 1 ? 128u : 64u), false);
            makeTarget(backgroundSmall, 256, 256, false);
            makeTarget(reflection, 256, 256, true);
            makeTarget(refraction, 256, 256, false);
            makeTarget(hazeSmall, 256, 256, false);
            for (Target &t : prefix)
                if (t.color.image)
                    makeTarget(t, w, h, true);
            makeTarget(refractionHalf, 128, 128, false);
            for (Target &t : bloomTex)
                makeTarget(t, std::max(w / 4u, 1u), std::max(h / 4u, 1u), false);
            for (int i = 0; i < 7; ++i)
            {
                const uint32_t size = (i == 0 || i == 6) ? 256u : ((i == 1 || i == 5) ? 128u : 64u);
                makeTarget(gsBloom[i], size, size, false);
            }
            gsBloomHistory = false;
            colorPlain = Fb{&color.color, nullptr};
            shadowMaskFb = Fb{&shadowMask.color, &color.depth}; // the shadow mask pass tests the picture's stencil
            glowLayerFb = Fb{&glowLayer.color, &color.depth};   // layer 8 drawn apart on the picture's depth
            bloomHistory = false;
            width = w;
            height = h;
            if (!direct && (!readback.buffer || readback.size < static_cast<VkDeviceSize>(w) * h * 4u))
                makeStream(readback, static_cast<VkDeviceSize>(w) * h * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
            std::fprintf(stderr, "[native:vk] targets %ux%u\n", w, h);
        }

        VkShaderModule shader(const uint32_t *code, size_t bytes)
        {
            VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            ci.codeSize = bytes;
            ci.pCode = code;
            VkShaderModule m = VK_NULL_HANDLE;
            check(vkCreateShaderModule(device, &ci, nullptr, &m), "vkCreateShaderModule");
            return m;
        }
        VkRenderPass makeRenderPass(bool withDepth)
        {
            VkAttachmentDescription a[2]{};
            a[0].format = VK_FORMAT_R8G8B8A8_UNORM;
            a[0].samples = VK_SAMPLE_COUNT_1_BIT;
            a[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            a[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            a[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a[0].initialLayout = a[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            a[1] = a[0];
            a[1].format = depthFormat;
            a[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            a[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            a[1].initialLayout = a[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            VkAttachmentReference c{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, d{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
            VkSubpassDescription s{};
            s.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            s.colorAttachmentCount = 1;
            s.pColorAttachments = &c;
            s.pDepthStencilAttachment = withDepth ? &d : nullptr;
            VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            ci.attachmentCount = withDepth ? 2u : 1u;
            ci.pAttachments = a;
            ci.subpassCount = 1;
            ci.pSubpasses = &s;
            VkRenderPass rp = VK_NULL_HANDLE;
            check(vkCreateRenderPass(device, &ci, nullptr, &rp), "vkCreateRenderPass");
            return rp;
        }
        VkSampler makeSampler(VkFilter mag, VkFilter min, VkSamplerMipmapMode mip, bool clampS, bool clampT, float aniso)
        {
            VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            ci.magFilter = mag;
            ci.minFilter = min;
            ci.mipmapMode = mip;
            ci.addressModeU = clampS ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
            ci.addressModeV = clampT ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
            ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            ci.anisotropyEnable = aniso > 1.0f ? VK_TRUE : VK_FALSE;
            ci.maxAnisotropy = std::max(1.0f, aniso);
            ci.minLod = 0.0f;
            ci.maxLod = VK_LOD_CLAMP_NONE;
            VkSampler s = VK_NULL_HANDLE;
            check(vkCreateSampler(device, &ci, nullptr, &s), "vkCreateSampler");
            return s;
        }
        // The GL backend's texture filtering (Quality "Texture filtering"): nearest, bilinear, trilinear,
        // anisotropic 2x-16x; with the texture's wrap (the GS's CLAMP).
        VkSampler gameSampler(uint32_t clamp, uint32_t filter, bool mipped)
        {
            filter = std::min(filter, 6u);
            const uint32_t key = (clamp & 3u) | (filter << 2) | (mipped ? 32u : 0u);
            if (!gameSamplers[key])
            {
                const VkFilter f = filter == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
                const VkSamplerMipmapMode mip = filter >= 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
                const float aniso = anisotropy && filter >= 3u && mipped ? std::min(maxAnisotropy, static_cast<float>(1u << (filter - 2u))) : 1.0f;
                gameSamplers[key] = makeSampler(f, f, mip, clamp & 1u, clamp & 2u, aniso);
            }
            return gameSamplers[key];
        }

        VkPipeline makePipeline(VkShaderModule vs, VkShaderModule fs, VkPipelineLayout layout, VkRenderPass rp, int vertices,
                                const VkPipelineColorBlendAttachmentState &blend, const VkPipelineDepthStencilStateCreateInfo &ds, bool clampDepth)
        {
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = vs;
            stages[0].pName = "main";
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = fs;
            stages[1].pName = "main";
            VkVertexInputBindingDescription binding{0, 0, VK_VERTEX_INPUT_RATE_VERTEX};
            VkVertexInputAttributeDescription attrs[6]{};
            uint32_t attrCount = 0;
            if (vertices)
            {
                using V = native_renderer::Vertex;
                using S = native_renderer::SkinVertex;
                const bool skin = vertices == 2;
                binding.stride = skin ? sizeof(S) : sizeof(V);
                attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(skin ? offsetof(S, x) : offsetof(V, x))};
                attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(skin ? offsetof(S, nx) : offsetof(V, nx))};
                attrs[2] = {2, 0, VK_FORMAT_R8G8B8A8_UINT, static_cast<uint32_t>(skin ? offsetof(S, r) : offsetof(V, r))};
                attrs[3] = {3, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(skin ? offsetof(S, u) : offsetof(V, u))};
                attrCount = 4;
                if (skin)
                {
                    attrs[4] = {4, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(S, bone))};
                    attrs[5] = {5, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(S, weight))};
                    attrCount = 6;
                }
            }
            VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            vi.vertexBindingDescriptionCount = vertices ? 1u : 0u;
            vi.pVertexBindingDescriptions = &binding;
            vi.vertexAttributeDescriptionCount = attrCount;
            vi.pVertexAttributeDescriptions = attrs;
            VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            vp.viewportCount = vp.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            rs.depthClampEnable = clampDepth && depthClamp ? VK_TRUE : VK_FALSE;
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.cullMode = VK_CULL_MODE_NONE;
            // GL's counter-clockwise front, seen in this picture's rows (row 0 at the top in Vulkan's terms).
            rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
            rs.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            cb.attachmentCount = 1;
            cb.pAttachments = &blend;
            const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dy.dynamicStateCount = 2;
            dy.pDynamicStates = dyn;
            VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            ci.stageCount = 2;
            ci.pStages = stages;
            ci.pVertexInputState = &vi;
            ci.pInputAssemblyState = &ia;
            ci.pViewportState = &vp;
            ci.pRasterizationState = &rs;
            ci.pMultisampleState = &ms;
            ci.pDepthStencilState = &ds;
            ci.pColorBlendState = &cb;
            ci.pDynamicState = &dy;
            ci.layout = layout;
            ci.renderPass = rp;
            VkPipeline p = VK_NULL_HANDLE;
            check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &p), "vkCreateGraphicsPipelines");
            return p;
        }
        static VkPipelineColorBlendAttachmentState blendState(bool on, VkBlendFactor src, VkBlendFactor dst, VkBlendOp op, VkBlendFactor srcA,
                                                              VkBlendFactor dstA)
        {
            VkPipelineColorBlendAttachmentState b{};
            b.blendEnable = on ? VK_TRUE : VK_FALSE;
            b.srcColorBlendFactor = src;
            b.dstColorBlendFactor = dst;
            b.colorBlendOp = op;
            b.srcAlphaBlendFactor = srcA;
            b.dstAlphaBlendFactor = dstA;
            b.alphaBlendOp = VK_BLEND_OP_ADD;
            b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            return b;
        }
        // A main draw's pipeline: the GL backend's setBlend (alpha: the source kept) and depth state.
        VkPipeline mainPipeline(bool skinned, native_renderer::Blend blend, bool depthTest, bool depthWrite, bool shadowVolume)
        {
            uint32_t key = (skinned ? 1u : 0u) | (shadowVolume ? 2u : (static_cast<uint32_t>(blend) & 3u) << 2 | (depthTest ? 16u : 0u) | (depthWrite ? 32u : 0u));
            if (mainPipelines[key])
                return mainPipelines[key];
            VkPipelineColorBlendAttachmentState b{};
            switch (blend)
            {
            case native_renderer::Blend::Opaque:
                b = blendState(false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO);
                break;
            case native_renderer::Blend::Additive:
                b = blendState(true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO);
                break;
            case native_renderer::Blend::Subtract:
                b = blendState(true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO);
                break;
            default:
                b = blendState(true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE,
                               VK_BLEND_FACTOR_ZERO);
                break;
            }
            VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL; // the GS's GEQUAL: a surface drawn again in place passes
            if (shadowVolume)
            {
                // The count where the faces pass the depth test: front +1, back -1; no colour; not cut at the near plane.
                b = blendState(false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO);
                b.colorWriteMask = 0;
                ds.depthTestEnable = VK_TRUE;
                ds.depthWriteEnable = VK_FALSE;
                ds.stencilTestEnable = VK_TRUE;
                ds.front = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0xFF, 0xFF, 0};
                ds.back = ds.front;
                ds.back.passOp = VK_STENCIL_OP_DECREMENT_AND_WRAP;
            }
            else
            {
                ds.depthTestEnable = depthTest ? VK_TRUE : VK_FALSE;
                ds.depthWriteEnable = depthTest && depthWrite ? VK_TRUE : VK_FALSE;
            }
            mainPipelines[key] = makePipeline(skinned ? skinVert : mainVert, mainFrag, mainLayout, rpColorDepth, skinned ? 2 : 1, b, ds, shadowVolume);
            return mainPipelines[key];
        }
        enum class QuadBlend
        {
            None,
            Add,    // Cs + Cd, alpha kept
            Over,   // Cs x As + Cd x (1 - As), alpha kept
            Darken, // Cd x (1 - As)
        };
        VkPipeline quadPipeline(bool withDepth, QuadBlend blend, bool stencil)
        {
            VkPipeline &p = quadPipelines[withDepth ? 1 : 0][static_cast<int>(blend)][stencil ? 1 : 0];
            if (p)
                return p;
            VkPipelineColorBlendAttachmentState b{};
            switch (blend)
            {
            case QuadBlend::None:
                b = blendState(false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO);
                break;
            case QuadBlend::Add:
                b = blendState(true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE);
                break;
            case QuadBlend::Over:
                b = blendState(true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ZERO,
                               VK_BLEND_FACTOR_ONE);
                break;
            case QuadBlend::Darken:
                b = blendState(true, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ZERO,
                               VK_BLEND_FACTOR_ONE);
                break;
            }
            VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            if (stencil) // where the count is not 0, and the count cleared there
            {
                ds.stencilTestEnable = VK_TRUE;
                ds.front = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_NOT_EQUAL, 0xFF, 0xFF, 0};
                ds.back = ds.front;
            }
            p = makePipeline(quadVert, quadFrag, quadLayout, withDepth ? rpColorDepth : rpColor, 0, b, ds, false);
            return p;
        }

        // =============================== recording ===============================
        void barrier(Image &img, VkImageLayout layout)
        {
            // Everything before, then the new use: the targets change use a few dozen times a frame.
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            b.oldLayout = img.layout;
            b.newLayout = layout;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img.image;
            b.subresourceRange = {static_cast<VkImageAspectFlags>(img.depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_COLOR_BIT),
                                  0, img.levels, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
            img.layout = layout;
        }
        static VkImageLayout readLayout(const Image &img)
        {
            return img.depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        void endPass()
        {
            if (!inPass)
                return;
            vkCmdEndRenderPass(cmd);
            inPass = false;
            passColor = passDepth = VK_NULL_HANDLE;
        }
        // Ready to be read by a shader (outside the render pass: one in progress ends).
        void readable(Image &img)
        {
            if (img.layout == readLayout(img))
                return;
            endPass();
            barrier(img, readLayout(img));
        }
        VkFramebuffer framebuffer(const Fb &f)
        {
            const auto key = std::make_pair(f.color->view, f.depth ? f.depth->attach : VK_NULL_HANDLE);
            auto it = framebuffers.find(key);
            if (it != framebuffers.end())
                return it->second;
            VkImageView views[2] = {f.color->view, f.depth ? f.depth->attach : VK_NULL_HANDLE};
            VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            ci.renderPass = f.depth ? rpColorDepth : rpColor;
            ci.attachmentCount = f.depth ? 2u : 1u;
            ci.pAttachments = views;
            ci.width = f.color->width;
            ci.height = f.color->height;
            ci.layers = 1;
            VkFramebuffer fbo = VK_NULL_HANDLE;
            check(vkCreateFramebuffer(device, &ci, nullptr, &fbo), "vkCreateFramebuffer");
            framebuffers.emplace(key, fbo);
            return fbo;
        }
        // Drawing into `f` (the GL backend's bindTarget): the render pass in progress when it is the same, else a new
        // one that loads what the target holds.
        void beginPass(const Fb &f)
        {
            const VkImage c = f.color->image, d = f.depth ? f.depth->image : VK_NULL_HANDLE;
            if (inPass && passColor == c && passDepth == d)
                return;
            endPass();
            barrier(*f.color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            if (f.depth)
                barrier(*f.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            bi.renderPass = f.depth ? rpColorDepth : rpColor;
            bi.framebuffer = framebuffer(f);
            bi.renderArea = {{0, 0}, {f.color->width, f.color->height}};
            vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
            const VkViewport vp{0.0f, 0.0f, static_cast<float>(f.color->width), static_cast<float>(f.color->height), 0.0f, 1.0f};
            const VkRect2D sc{{0, 0}, {f.color->width, f.color->height}};
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);
            inPass = true;
            passColor = c;
            passDepth = d;
            boundPipeline = VK_NULL_HANDLE;
            ++stats.targets;
        }
        // glClear inside the target being drawn.
        void clear(const Fb &f, const float *rgba, bool depthStencil)
        {
            beginPass(f);
            VkClearAttachment at[2]{};
            uint32_t n = 0;
            if (rgba)
            {
                at[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                at[n].colorAttachment = 0;
                std::memcpy(at[n].clearValue.color.float32, rgba, 4 * sizeof(float));
                ++n;
            }
            if (depthStencil && f.depth)
            {
                at[n].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
                at[n].clearValue.depthStencil = {1.0f, 0};
                ++n;
            }
            if (!n)
                return;
            VkClearRect rect{{{0, 0}, {f.color->width, f.color->height}}, 0, 1};
            vkCmdClearAttachments(cmd, n, at, 1, &rect);
        }
        void bindPipeline(VkPipeline p)
        {
            if (p == boundPipeline)
                return;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
            boundPipeline = p;
        }
        VkDescriptorSet allocateSet(VkDescriptorSetLayout layout)
        {
            for (;;)
            {
                if (descriptorPool >= descriptorPools.size())
                {
                    const VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192},
                                                           {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 256},
                                                           {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 256}};
                    VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
                    ci.maxSets = 2048;
                    ci.poolSizeCount = 3;
                    ci.pPoolSizes = sizes;
                    VkDescriptorPool pool = VK_NULL_HANDLE;
                    if (!check(vkCreateDescriptorPool(device, &ci, nullptr, &pool), "vkCreateDescriptorPool"))
                        return VK_NULL_HANDLE;
                    descriptorPools.push_back(pool);
                }
                VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                ai.descriptorPool = descriptorPools[descriptorPool];
                ai.descriptorSetCount = 1;
                ai.pSetLayouts = &layout;
                VkDescriptorSet set = VK_NULL_HANDLE;
                const VkResult r = vkAllocateDescriptorSets(device, &ai, &set);
                if (r == VK_SUCCESS)
                    return set;
                if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL)
                {
                    check(r, "vkAllocateDescriptorSets");
                    return VK_NULL_HANDLE;
                }
                ++descriptorPool;
            }
        }
        VkDescriptorSet imageSet(VkDescriptorSetLayout layout, const Binding *b, uint32_t n)
        {
            VkDescriptorSet set = allocateSet(layout);
            if (!set)
                return set;
            VkDescriptorImageInfo info[4];
            VkWriteDescriptorSet w[4];
            for (uint32_t i = 0; i < n; ++i)
            {
                info[i] = {b[i].sampler, b[i].view, b[i].layout};
                w[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w[i].dstSet = set;
                w[i].dstBinding = i;
                w[i].descriptorCount = 1;
                w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[i].pImageInfo = &info[i];
            }
            vkUpdateDescriptorSets(device, n, w, 0, nullptr);
            return set;
        }
        // A render target to be read (made readable), or the dummy when it is being drawn into (as the GL backend's
        // stale units: the shader does not read it then).
        Binding sampleTarget(Image *img, const Fb &target)
        {
            if (!img || !img->image || img->image == target.color->image || (target.depth && img->image == target.depth->image))
                return Binding{dummy.view, linearClamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            readable(*img);
            return Binding{img->view, img->depth ? nearestClamp : linearClamp, readLayout(*img)};
        }
        // The per-draw values into this frame's uniform chunks: the offset, and frameSet for its chunk.
        uint32_t pushUniforms(const DrawUniforms &u)
        {
            if (!frameSet || uniformUsed + uniformStride > kUniformChunk)
            {
                if (frameSet)
                    ++uniformChunk;
                if (uniformChunk >= uniformChunks.size())
                {
                    uniformChunks.emplace_back();
                    makeStream(uniformChunks.back(), kUniformChunk, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
                }
                uniformUsed = 0;
                frameSet = allocateSet(frameLayout);
                if (frameSet)
                {
                    VkDescriptorBufferInfo ub{uniformChunks[uniformChunk].buffer, 0, sizeof(DrawUniforms)};
                    VkDescriptorBufferInfo sb{paletteStream.buffer, 0, VK_WHOLE_SIZE};
                    VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
                    w[0].dstSet = w[1].dstSet = frameSet;
                    w[0].dstBinding = 0;
                    w[0].descriptorCount = 1;
                    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                    w[0].pBufferInfo = &ub;
                    w[1].dstBinding = 1;
                    w[1].descriptorCount = 1;
                    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    w[1].pBufferInfo = &sb;
                    vkUpdateDescriptorSets(device, 2, w, 0, nullptr);
                }
            }
            const uint32_t offset = static_cast<uint32_t>(uniformUsed);
            std::memcpy(uniformChunks[uniformChunk].mapped + offset, &u, sizeof(u));
            uniformUsed += uniformStride;
            return offset;
        }
        void bindVertices(VkBuffer buffer, VkDeviceSize offset)
        {
            if (buffer == boundVertexBuffer && offset == boundVertexOffset)
                return;
            vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &offset);
            boundVertexBuffer = buffer;
            boundVertexOffset = offset;
        }
        void bindIndices(VkBuffer buffer, VkDeviceSize offset)
        {
            if (buffer == boundIndexBuffer && offset == boundIndexOffset)
                return;
            vkCmdBindIndexBuffer(cmd, buffer, offset, VK_INDEX_TYPE_UINT32);
            boundIndexBuffer = buffer;
            boundIndexOffset = offset;
        }

        // A full-screen pass into `target` (the GL backend's quad): its textures are unit[0..2] (bindTexture).
        void quad(const Fb &target, int mode, QuadBlend blend, const float a[4] = nullptr, const float b[4] = nullptr,
                  const float quadParams[4] = nullptr, bool corners = false, bool stencil = false)
        {
            const bool src = !(mode == 5 || mode == 6 || mode == 8 || mode == 15);
            const bool src2 = mode == 2 || mode == 17;
            const bool depthIn = mode == 8 || mode == 11 || mode == 12 || mode == 15 || mode == 18;
            // The textures first (a target read ends the render pass it was drawn in), then the target.
            Binding set[3];
            const Fb t = target;
            auto pick = [&](bool used, Image *img) -> Binding
            {
                if (!used || !img || img->image == t.color->image || (t.depth && img->image == t.depth->image))
                    return Binding{dummy.view, linearClamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                readable(*img);
                return Binding{img->view, img->depth ? nearestClamp : linearClamp, readLayout(*img)};
            };
            set[0] = pick(src, unit[0]);
            set[1] = pick(src2, unit[1]);
            set[2] = pick(depthIn, unit[2]);
            beginPass(t);
            bindPipeline(quadPipeline(t.depth != nullptr, blend, stencil));
            if (!lastQuadSet || !(set[0] == lastQuad[0] && set[1] == lastQuad[1] && set[2] == lastQuad[2]))
            {
                lastQuadSet = imageSet(quadSetLayout, set, 3);
                for (int i = 0; i < 3; ++i)
                    lastQuad[i] = set[i];
            }
            if (!lastQuadSet)
                return;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, quadLayout, 0, 1, &lastQuadSet, 0, nullptr);
            QuadPush p{};
            if (a)
                std::memcpy(p.a, a, sizeof(p.a));
            if (b)
                std::memcpy(p.b, b, sizeof(p.b));
            if (quadParams)
                std::memcpy(p.quad, quadParams, sizeof(p.quad));
            p.mode = mode;
            p.corners = corners ? 1 : 0;
            vkCmdPushConstants(cmd, quadLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
            vkCmdDraw(cmd, corners ? 6u : 3u, 1, 0, 0);
        }
        void bindTexture(int u, Image &img) { unit[u] = &img; }
        void bindTexture(int u, Target &t) { unit[u] = &t.color; }
        // The colour of `from` into `to` (bilinear: reducing blurs it).
        void copy(Target &from, Target &to)
        {
            bindTexture(0, from);
            quad(fb(to), 0, QuadBlend::None);
        }
        void copy(Target &from, const Fb &to)
        {
            bindTexture(0, from);
            quad(to, 0, QuadBlend::None);
        }
        void snapshotColor() { copy(color, snapshot); }
        void blur(int rounds, float spreadGrow = 0.0f)
        {
            const float lw = static_cast<float>(bloomTex[0].width), lh = static_cast<float>(bloomTex[0].height);
            for (int i = 0; i < rounds; ++i)
            {
                const float spread = 1.0f + spreadGrow * static_cast<float>(i);
                const float k = spread * static_cast<float>(width) / 512.0f;
                const float h[4] = {k / lw, 0, 0, 0}, v[4] = {0, k / lh, 0, 0};
                bindTexture(0, bloomTex[0]);
                quad(fb(bloomTex[1]), 1, QuadBlend::None, h);
                bindTexture(0, bloomTex[1]);
                quad(fb(bloomTex[0]), 1, QuadBlend::None, v);
            }
        }

        // ---- a command buffer: one per frame (and per batch of uploads between frames) ----
        bool beginCommands()
        {
            if (lost)
                return false;
            check(vkResetCommandPool(device, commandPool, 0), "vkResetCommandPool");
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (!check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer"))
                return false;
            for (VkDescriptorPool p : descriptorPools)
                vkResetDescriptorPool(device, p, 0);
            descriptorPool = 0;
            uniformChunk = 0;
            uniformUsed = 0;
            frameSet = VK_NULL_HANDLE;
            lastTextureSet = lastQuadSet = VK_NULL_HANDLE;
            boundPipeline = VK_NULL_HANDLE;
            boundVertexBuffer = boundIndexBuffer = VK_NULL_HANDLE;
            boundVertexOffset = boundIndexOffset = ~0ull;
            inPass = false;
            passColor = passDepth = VK_NULL_HANDLE;
            recording = true;
            return true;
        }
        bool submitAndWait()
        {
            if (!recording)
                return false;
            endPass();
            recording = false;
            if (!check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer"))
                return false;
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            if (presentNext)
            {
                si.waitSemaphoreCount = 1;
                si.pWaitSemaphores = &acquired;
                si.pWaitDstStageMask = &waitStage;
                si.signalSemaphoreCount = 1;
                si.pSignalSemaphores = &drawn;
            }
            VkResult r = vkQueueSubmit(queue, 1, &si, fence);
            if (r == VK_SUCCESS)
                r = vkWaitForFences(device, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000);
            vkResetFences(device, 1, &fence);
            if (r != VK_SUCCESS)
            {
                check(r, "the frame's submission");
                if (r == VK_ERROR_DEVICE_LOST || r == VK_TIMEOUT)
                {
                    lost = true;
                    std::fprintf(stderr, "[native:vk] the GPU stopped: no more frames from the Vulkan renderer\n");
                }
                return false;
            }
            return true;
        }

        void evictUnused();
        void freeMesh(Mesh &m)
        {
            release(m.vertices);
            release(m.indices);
        }
        bool uploadMesh(uint64_t id, const native_renderer::Vertex *vertices, uint32_t count);
        bool uploadSkinned(uint64_t id, const native_renderer::SkinVertex *vertices, uint32_t count, const uint32_t *indices, uint32_t indexCount);
        void applyUploadList(std::vector<Upload> &list);
        void applyUploads();
        void render(uint32_t w, uint32_t h);
        void finishReadback();
        // The render thread's work for one frame / for uploads alone.
        // Where the render thread's time goes (logged every 120 frames): uploads and recording, the submission
        // and the wait for the GPU, the read-back.
        struct
        {
            uint64_t frames = 0, recordUs = 0, gpuUs = 0, readbackUs = 0, worstUs = 0;
        } timing;
        void frame(uint32_t w, uint32_t h)
        {
            const uint64_t t0 = native_renderer::nowUs();
            if (!beginCommands())
                return;
            applyUploads();
            presentNext = false;
            if (!hold)
            {
                render(w, h);
                if (direct && native_renderer::tuning().showNative)
                {
                    presentCpu = false;
                    recordPresent();
                }
            }
            const uint64_t t1 = native_renderer::nowUs();
            const bool ok = submitAndWait();
            const uint64_t t2 = native_renderer::nowUs();
            if (ok && presentNext)
            {
                queuePresent();
                lastNativePresentUs = lastPresentUs;
            }
            if (ok && !hold && !direct)
                finishReadback();
            const uint64_t t3 = native_renderer::nowUs();
            timing.recordUs += t1 - t0;
            timing.gpuUs += t2 - t1;
            timing.readbackUs += t3 - t2;
            timing.worstUs = std::max(timing.worstUs, t3 - t0);
            if (++timing.frames == 120)
            {
                std::fprintf(stderr, "[native:vk] 120 frames: record %.2f ms, submit and GPU %.2f ms, read back %.2f ms a frame; slowest %.1f ms\n",
                             timing.recordUs / 120e3, timing.gpuUs / 120e3, timing.readbackUs / 120e3, timing.worstUs / 1e3);
                timing = {};
            }
        }
        void uploadsAlone(std::vector<Upload> &list)
        {
            if (!beginCommands())
            {
                list.clear();
                return;
            }
            applyUploadList(list);
            submitAndWait();
        }
    };

    bool Renderer::init()
    {
#if RC_VK_STATIC_ICD
        vkGetInstanceProcAddrFn = vk_icdGetInstanceProcAddr;
#else
        void *library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!library)
        {
            std::fprintf(stderr, "[native:vk] no libvulkan.so.1\n");
            return false;
        }
        vkGetInstanceProcAddrFn = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
#endif
        if (!vkGetInstanceProcAddrFn)
            return false;
        vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(vkGetInstanceProcAddrFn(VK_NULL_HANDLE, "vkCreateInstance"));
        if (!vkCreateInstance)
        {
            std::fprintf(stderr, "[native:vk] no vkCreateInstance\n");
            return false;
        }
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "reColossus";
        app.pEngineName = "Celosia native renderer";
        app.apiVersion = VK_API_VERSION_1_1; // external memory (the PS5's swapchain imports VideoOut's buffers) in the core
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        // The display: the PS5's own (VK_KHR_display), unless RECOLOSSUS_PRESENT=host (the picture read back and
        // shown by the host, as up to 0.2.3).
        const char *instanceExtensions[2] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
        {
            const char *present = std::getenv("RECOLOSSUS_PRESENT");
#if RC_VK_STATIC_ICD
            const bool wantDirect = !(present && std::strcmp(present, "host") == 0);
#else
            // the Linux check (tests/vk_check.sh): RECOLOSSUS_PRESENT=headless presents to a surface of no window
            headless = present && std::strcmp(present, "headless") == 0;
            if (headless)
                instanceExtensions[1] = "VK_EXT_headless_surface";
            const bool wantDirect = headless || (present && std::strcmp(present, "display") == 0);
#endif
            vkEnumerateInstanceExtensionProperties = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
                vkGetInstanceProcAddrFn(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
            if (wantDirect && vkEnumerateInstanceExtensionProperties)
            {
                uint32_t n = 0;
                vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
                std::vector<VkExtensionProperties> ext(n);
                if (n)
                    vkEnumerateInstanceExtensionProperties(nullptr, &n, ext.data());
                int found = 0;
                for (const VkExtensionProperties &e : ext)
                    for (const char *want : instanceExtensions)
                        found += std::strcmp(e.extensionName, want) == 0 ? 1 : 0;
                displayExtensions = found == 2;
                std::fprintf(stderr, "[native:vk] display extensions %s\n", displayExtensions ? "there" : "missing: the host shows the picture");
            }
        }
        if (displayExtensions)
        {
            ici.enabledExtensionCount = 2;
            ici.ppEnabledExtensionNames = instanceExtensions;
        }
        if (!check(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance"))
            return false;
        bool missing = false;
#define RC_VK_LOAD_INSTANCE(name)                                                                                       \
    name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddrFn(instance, #name));                                      \
    if (!name)                                                                                                          \
    {                                                                                                                   \
        std::fprintf(stderr, "[native:vk] no %s\n", #name);                                                             \
        missing = true;                                                                                                 \
    }
        RC_VK_INSTANCE_FUNCS(RC_VK_LOAD_INSTANCE)
#undef RC_VK_LOAD_INSTANCE
        if (missing)
            return false;
#define RC_VK_LOAD_OPTIONAL(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddrFn(instance, #name));
        RC_VK_DISPLAY_INSTANCE_FUNCS(RC_VK_LOAD_OPTIONAL)
#undef RC_VK_LOAD_OPTIONAL
        uint32_t n = 0;
        vkEnumeratePhysicalDevices(instance, &n, nullptr);
        std::vector<VkPhysicalDevice> gpus(n);
        if (n)
            vkEnumeratePhysicalDevices(instance, &n, gpus.data());
        for (VkPhysicalDevice g : gpus)
        {
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> q(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, q.data());
            for (uint32_t i = 0; i < qn; ++i)
                if (q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                {
                    gpu = g;
                    queueFamily = i;
                    break;
                }
            if (gpu)
                break;
        }
        if (!gpu)
        {
            std::fprintf(stderr, "[native:vk] no Vulkan device with graphics\n");
            return false;
        }
        vkGetPhysicalDeviceProperties(gpu, &props);
        vkGetPhysicalDeviceMemoryProperties(gpu, &memProps);
        VkPhysicalDeviceFeatures have{}, want{};
        vkGetPhysicalDeviceFeatures(gpu, &have);
        want.depthClamp = have.depthClamp;
        want.samplerAnisotropy = have.samplerAnisotropy;
        depthClamp = have.depthClamp == VK_TRUE;
        anisotropy = have.samplerAnisotropy == VK_TRUE;
        maxAnisotropy = anisotropy ? props.limits.maxSamplerAnisotropy : 1.0f;
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = queueFamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.pEnabledFeatures = &want;
        // The swapchain, and host memory imports (the PS5's WSI takes VideoOut's buffers in as host memory).
        std::vector<const char *> deviceExtensions;
        if (displayExtensions && vkEnumerateDeviceExtensionProperties)
        {
            uint32_t n = 0;
            vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, nullptr);
            std::vector<VkExtensionProperties> ext(n);
            if (n)
                vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, ext.data());
            bool swap = false;
            for (const VkExtensionProperties &e : ext)
            {
                if (std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0)
                    swap = true;
                else if (std::strcmp(e.extensionName, "VK_EXT_external_memory_host") == 0)
                    deviceExtensions.push_back("VK_EXT_external_memory_host");
            }
            if (swap)
                deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
            else
            {
                displayExtensions = false;
                deviceExtensions.clear();
                std::fprintf(stderr, "[native:vk] no VK_KHR_swapchain: the host shows the picture\n");
            }
        }
        dci.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
        dci.ppEnabledExtensionNames = deviceExtensions.empty() ? nullptr : deviceExtensions.data();
        if (!check(vkCreateDevice(gpu, &dci, nullptr, &device), "vkCreateDevice"))
            return false;
#define RC_VK_LOAD_DEVICE(name)                                                                                         \
    name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));                                            \
    if (!name)                                                                                                          \
    {                                                                                                                   \
        std::fprintf(stderr, "[native:vk] no %s\n", #name);                                                             \
        missing = true;                                                                                                 \
    }
        RC_VK_DEVICE_FUNCS(RC_VK_LOAD_DEVICE)
#undef RC_VK_LOAD_DEVICE
        if (missing)
            return false;
        if (displayExtensions)
        {
#define RC_VK_LOAD_OPTIONAL(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));
            RC_VK_DISPLAY_DEVICE_FUNCS(RC_VK_LOAD_OPTIONAL)
#undef RC_VK_LOAD_OPTIONAL
        }
        vkGetDeviceQueue(device, queueFamily, 0, &queue);
        std::fprintf(stderr, "[native:vk] %s, Vulkan %u.%u.%u, driver 0x%x, depth clamp %d, anisotropy %.0f\n", props.deviceName,
                     VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion), props.driverVersion,
                     depthClamp ? 1 : 0, maxAnisotropy);
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
            std::fprintf(stderr, "[native:vk] memory type %u: flags 0x%x heap %u (%llu MB)\n", i, memProps.memoryTypes[i].propertyFlags,
                         memProps.memoryTypes[i].heapIndex,
                         static_cast<unsigned long long>(memProps.memoryHeaps[memProps.memoryTypes[i].heapIndex].size >> 20));

        // The depth / stencil format (32-bit float depth as the GL backend's; 24-bit when the GPU has no other).
        for (VkFormat f : {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT})
        {
            VkFormatProperties fp{};
            vkGetPhysicalDeviceFormatProperties(gpu, f, &fp);
            const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            if ((fp.optimalTilingFeatures & need) == need)
            {
                depthFormat = f;
                break;
            }
        }
        uniformStride = (sizeof(DrawUniforms) + props.limits.minUniformBufferOffsetAlignment - 1u) / props.limits.minUniformBufferOffsetAlignment *
                        props.limits.minUniformBufferOffsetAlignment;

        VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        cpi.queueFamilyIndex = queueFamily;
        if (!check(vkCreateCommandPool(device, &cpi, nullptr, &commandPool), "vkCreateCommandPool"))
            return false;
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = commandPool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        if (!check(vkAllocateCommandBuffers(device, &cai, &cmd), "vkAllocateCommandBuffers"))
            return false;
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (!check(vkCreateFence(device, &fci, nullptr, &fence), "vkCreateFence"))
            return false;

        rpColor = makeRenderPass(false);
        rpColorDepth = makeRenderPass(true);
        {
            VkDescriptorSetLayoutBinding b[4]{};
            b[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
            b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            ci.bindingCount = 2;
            ci.pBindings = b;
            check(vkCreateDescriptorSetLayout(device, &ci, nullptr, &frameLayout), "vkCreateDescriptorSetLayout");
            for (uint32_t i = 0; i < 4; ++i)
                b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
            ci.bindingCount = 4;
            check(vkCreateDescriptorSetLayout(device, &ci, nullptr, &textureLayout), "vkCreateDescriptorSetLayout");
            ci.bindingCount = 3;
            check(vkCreateDescriptorSetLayout(device, &ci, nullptr, &quadSetLayout), "vkCreateDescriptorSetLayout");
            const VkDescriptorSetLayout mainSets[2] = {frameLayout, textureLayout};
            VkPushConstantRange mainPush{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(uint32_t)};
            VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            li.setLayoutCount = 2;
            li.pSetLayouts = mainSets;
            li.pushConstantRangeCount = 1;
            li.pPushConstantRanges = &mainPush;
            check(vkCreatePipelineLayout(device, &li, nullptr, &mainLayout), "vkCreatePipelineLayout");
            VkPushConstantRange quadPush{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(QuadPush)};
            li.setLayoutCount = 1;
            li.pSetLayouts = &quadSetLayout;
            li.pPushConstantRanges = &quadPush;
            check(vkCreatePipelineLayout(device, &li, nullptr, &quadLayout), "vkCreatePipelineLayout");
        }
        mainVert = shader(kVkMainVert, sizeof(kVkMainVert));
        skinVert = shader(kVkMainVertSkinned, sizeof(kVkMainVertSkinned));
        mainFrag = shader(kVkMainFrag, sizeof(kVkMainFrag));
        quadVert = shader(kVkQuadVert, sizeof(kVkQuadVert));
        quadFrag = shader(kVkQuadFrag, sizeof(kVkQuadFrag));
        if (!mainVert || !mainFrag || !quadVert || !quadFrag || !rpColor || !rpColorDepth || !mainLayout || !quadLayout)
            return false;
        linearClamp = makeSampler(VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, true, true, 1.0f);
        nearestClamp = makeSampler(VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, true, true, 1.0f);

        // Kept meshes: one pool of buffers the CPU writes (device local when the GPU shares the memory, as the PS5's).
        {
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = 1u << 20;
            bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
            VkBuffer probe = VK_NULL_HANDLE;
            if (!check(vkCreateBuffer(device, &bi, nullptr, &probe), "vkCreateBuffer (probe)"))
                return false;
            VkMemoryRequirements req{};
            vkGetBufferMemoryRequirements(device, probe, &req);
            vkDestroyBuffer(device, probe, nullptr);
            const uint32_t type = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (type == ~0u)
                return false;
            meshPool = makePool(type, true, bi.usage);
        }
        const VkBufferUsageFlags vb = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, ib = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        if (!makeStream(vertexStream, 1u << 20, vb) || !makeStream(indexStream, 1u << 18, ib) ||
            !makeStream(paletteStream, 1u << 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || !makeStream(staging, 4u << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
            return false;
        // The dummy texture, black.
        if (!makeImage(dummy, 1, 1, 1, false, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) || !beginCommands())
            return false;
        barrier(dummy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
        const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, dummy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
        barrier(dummy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (!submitAndWait())
            return false;
        // The pipelines most frames use, made now (the rest when first drawn with).
        for (int blend = 0; blend < 4; ++blend)
            for (int test = 0; test < 2; ++test)
                for (int write = 0; write < 2; ++write)
                    mainPipeline(false, static_cast<native_renderer::Blend>(blend), test != 0, write != 0, false);
        mainPipeline(false, native_renderer::Blend::Normal, true, false, true);
        skinOk = skinVert && mainPipeline(true, native_renderer::Blend::Normal, true, true, false) != VK_NULL_HANDLE;
        for (int d = 0; d < 2; ++d)
            for (int b = 0; b < 4; ++b)
                quadPipeline(d != 0, static_cast<QuadBlend>(b), false);
        if (!mainPipelines[(static_cast<uint32_t>(native_renderer::Blend::Normal) << 2) | 16u | 32u] || !quadPipelines[0][0][0])
        {
            std::fprintf(stderr, "[native:vk] the pipelines could not be made\n");
            return false;
        }
        std::fprintf(stderr, "[native:vk] up: depth format %d, uniform stride %llu, skinning on the GPU %d\n", static_cast<int>(depthFormat),
                     static_cast<unsigned long long>(uniformStride), skinOk ? 1 : 0);
        if (displayExtensions)
        {
            direct = setupDisplay();
            if (!direct)
                std::fprintf(stderr, "[native:vk] the display could not be set up: the host shows the picture (1920x1080)\n");
#if RC_VK_STATIC_ICD
            else
                rc_screen_gpu_owns();
#endif
        }
        return true;
    }

    // The PS5's display (PS5_Mesa's VK_KHR_display on VideoOut): the mode of the height asked for (RECOLOSSUS_OUTPUT,
    // 2160 by default; 1440 or 1080) at 60 Hz, a FIFO swapchain of B8G8R8A8, and what presents the picture onto it.
    bool Renderer::setupDisplay()
    {
        if (headless)
            return setupSwapchain(VkExtent2D{1280, 720});
        if (!vkGetPhysicalDeviceDisplayPropertiesKHR || !vkGetDisplayModePropertiesKHR || !vkCreateDisplayPlaneSurfaceKHR ||
            !vkGetPhysicalDeviceSurfaceCapabilitiesKHR || !vkGetPhysicalDeviceSurfaceFormatsKHR || !vkCreateSwapchainKHR ||
            !vkGetSwapchainImagesKHR || !vkAcquireNextImageKHR || !vkQueuePresentKHR || !vkCreateSemaphore)
        {
            std::fprintf(stderr, "[native:vk] display: entry points missing\n");
            return false;
        }
        uint32_t n = 0;
        vkGetPhysicalDeviceDisplayPropertiesKHR(gpu, &n, nullptr);
        if (!n)
        {
            std::fprintf(stderr, "[native:vk] display: none\n");
            return false;
        }
        std::vector<VkDisplayPropertiesKHR> displays(n);
        vkGetPhysicalDeviceDisplayPropertiesKHR(gpu, &n, displays.data());
        const VkDisplayKHR display = displays[0].display;
        uint32_t m = 0;
        vkGetDisplayModePropertiesKHR(gpu, display, &m, nullptr);
        std::vector<VkDisplayModePropertiesKHR> modes(m);
        if (m)
            vkGetDisplayModePropertiesKHR(gpu, display, &m, modes.data());
        modes.resize(m);
        if (modes.empty())
        {
            std::fprintf(stderr, "[native:vk] display: no modes\n");
            return false;
        }
        const char *out = std::getenv("RECOLOSSUS_OUTPUT");
        const uint32_t wantHeight = out && std::atoi(out) > 0 ? static_cast<uint32_t>(std::atoi(out)) : 2160u;
        // The tallest mode not over the height asked for (the smallest there is when all are), at about 60 Hz.
        const VkDisplayModePropertiesKHR *pick = nullptr;
        for (const VkDisplayModePropertiesKHR &mp : modes)
        {
            const VkExtent2D e = mp.parameters.visibleRegion;
            std::fprintf(stderr, "[native:vk] display mode %ux%u at %.2f Hz\n", e.width, e.height, mp.parameters.refreshRate / 1000.0);
            if (mp.parameters.refreshRate > 70000u)
                continue;
            if (!pick)
            {
                pick = &mp;
                continue;
            }
            const uint32_t have = pick->parameters.visibleRegion.height, h = e.height;
            const bool fits = h <= wantHeight, haveFits = have <= wantHeight;
            if ((fits && (!haveFits || h > have)) || (!fits && !haveFits && h < have))
                pick = &mp;
        }
        if (!pick)
            pick = &modes[0];
        VkDisplaySurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR};
        sci.displayMode = pick->displayMode;
        sci.planeIndex = 0;
        sci.planeStackIndex = 0;
        sci.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        sci.globalAlpha = 1.0f;
        sci.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
        sci.imageExtent = pick->parameters.visibleRegion;
        if (!check(vkCreateDisplayPlaneSurfaceKHR(instance, &sci, nullptr, &surface), "vkCreateDisplayPlaneSurfaceKHR"))
            return false;
        return setupSwapchain(pick->parameters.visibleRegion);
    }

    bool Renderer::setupSwapchain(VkExtent2D extent)
    {
#if !RC_VK_STATIC_ICD
        if (headless)
        {
            auto create = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(vkGetInstanceProcAddrFn(instance, "vkCreateHeadlessSurfaceEXT"));
            VkHeadlessSurfaceCreateInfoEXT hci{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
            if (!create || !check(create(instance, &hci, nullptr, &surface), "vkCreateHeadlessSurfaceEXT"))
                return false;
        }
#endif
        if (!vkGetPhysicalDeviceSurfaceCapabilitiesKHR || !vkGetPhysicalDeviceSurfaceFormatsKHR || !vkCreateSwapchainKHR || !vkGetSwapchainImagesKHR ||
            !vkAcquireNextImageKHR || !vkQueuePresentKHR || !vkCreateSemaphore)
            return false;
        if (vkGetPhysicalDeviceSurfaceSupportKHR)
        {
            VkBool32 supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(gpu, queueFamily, surface, &supported);
            if (!supported)
            {
                std::fprintf(stderr, "[native:vk] display: the queue cannot present to it\n");
                return false;
            }
        }
        VkSurfaceCapabilitiesKHR caps{};
        if (!check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
            return false;
        uint32_t fn = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &fn, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(fn);
        if (fn)
            vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &fn, formats.data());
        formats.resize(fn);
        if (formats.empty())
            return false;
        // UNORM: the picture is already in the display's encoding (as the GL backend's, shown as it is)
        VkSurfaceFormatKHR format = formats[0];
        for (const VkSurfaceFormatKHR &f : formats)
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM)
            {
                format = f;
                break;
            }
        swapchainFormat = format.format;
        screen = caps.currentExtent.width != 0xFFFFFFFFu ? caps.currentExtent : extent;
        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface = surface;
        ci.minImageCount = std::max(caps.minImageCount, 3u);
        if (caps.maxImageCount)
            ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
        ci.imageFormat = format.format;
        ci.imageColorSpace = format.colorSpace;
        ci.imageExtent = screen;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        ci.clipped = VK_TRUE;
        if (!check(vkCreateSwapchainKHR(device, &ci, nullptr, &swapchain), "vkCreateSwapchainKHR"))
            return false;
        uint32_t images = 0;
        vkGetSwapchainImagesKHR(device, swapchain, &images, nullptr);
        swapchainImages.resize(images);
        vkGetSwapchainImagesKHR(device, swapchain, &images, swapchainImages.data());
        swapchainImages.resize(images);

        // The present pass: the screen cleared to black, the picture in its rectangle, the overlay over all of it.
        VkAttachmentDescription a{};
        a.format = swapchainFormat;
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        a.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference c{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &c;
        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = 0;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpi.attachmentCount = 1;
        rpi.pAttachments = &a;
        rpi.subpassCount = 1;
        rpi.pSubpasses = &sub;
        rpi.dependencyCount = 1;
        rpi.pDependencies = &dep;
        if (!check(vkCreateRenderPass(device, &rpi, nullptr, &rpPresent), "vkCreateRenderPass (present)"))
            return false;
        for (VkImage img : swapchainImages)
        {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = img;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = swapchainFormat;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView view = VK_NULL_HANDLE;
            if (!check(vkCreateImageView(device, &vi, nullptr, &view), "vkCreateImageView (swapchain)"))
                return false;
            swapchainViews.push_back(view);
            VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fi.renderPass = rpPresent;
            fi.attachmentCount = 1;
            fi.pAttachments = &view;
            fi.width = screen.width;
            fi.height = screen.height;
            fi.layers = 1;
            VkFramebuffer f = VK_NULL_HANDLE;
            if (!check(vkCreateFramebuffer(device, &fi, nullptr, &f), "vkCreateFramebuffer (swapchain)"))
                return false;
            swapchainFramebuffers.push_back(f);
        }
        presentVert = shader(kVkPresentVert, sizeof(kVkPresentVert));
        presentFrag = shader(kVkPresentFrag, sizeof(kVkPresentFrag));
        if (!presentVert || !presentFrag)
            return false;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        presentPicture = makePipeline(presentVert, presentFrag, quadLayout, rpPresent, 0,
                                      blendState(false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO),
                                      ds, false);
        presentOverlay = makePipeline(presentVert, presentFrag, quadLayout, rpPresent, 0,
                                      blendState(true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ZERO,
                                                 VK_BLEND_FACTOR_ONE),
                                      ds, false);
        if (!presentPicture || !presentOverlay)
            return false;
        VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (!check(vkCreateSemaphore(device, &semi, nullptr, &acquired), "vkCreateSemaphore") ||
            !check(vkCreateSemaphore(device, &semi, nullptr, &drawn), "vkCreateSemaphore"))
            return false;

        // The host's overlay (its 1920x1080 frame, B8G8R8A8 with straight alpha), transparent to start with.
        int ow = 1920, oh = 1080;
#if RC_VK_STATIC_ICD
        rc_screen_overlay_size(&ow, &oh);
#endif
        if (!makeImage(overlay, static_cast<uint32_t>(ow), static_cast<uint32_t>(oh), 1, false,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_FORMAT_B8G8R8A8_UNORM) ||
            !makeStream(overlayStaging, static_cast<VkDeviceSize>(ow) * oh * 4u, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
            !makeStream(cpuStaging, 4u << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) || !beginCommands())
            return false;
        barrier(overlay, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkClearColorValue clear{{0.0f, 0.0f, 0.0f, 0.0f}};
        const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, overlay.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &all);
        barrier(overlay, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (!submitAndWait())
            return false;
        std::fprintf(stderr, "[native:vk] display %ux%u (format %d, %u images): the renderer presents, the picture at its own aspect\n", screen.width,
                     screen.height, static_cast<int>(swapchainFormat), images);
        return true;
    }

    // Recorded after the frame: the overlay's changes uploaded, a swapchain image acquired, and the present pass
    // (the picture fitted to the screen, the overlay over it). The submission waits for the image, then signals
    // `drawn` for vkQueuePresentKHR.
    void Renderer::recordPresent()
    {
        endPass();
#if RC_VK_STATIC_ICD
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (rc_screen_overlay_take(overlayStaging.mapped, static_cast<size_t>(overlayStaging.size), &x0, &y0, &x1, &y1) && x1 > x0 && y1 > y0)
        {
            barrier(overlay, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy region{};
            region.bufferRowLength = static_cast<uint32_t>(x1 - x0);
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageOffset = {x0, y0, 0};
            region.imageExtent = {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0), 1};
            vkCmdCopyBufferToImage(cmd, overlayStaging.buffer, overlay.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        overlaySeen = rc_screen_overlay_serial();
#endif
        Image &picture = presentCpu ? cpuPicture : color.color;
        if (!picture.image)
            return;
        readable(picture);
        readable(overlay);
        const VkResult r = vkAcquireNextImageKHR(device, swapchain, 1000ull * 1000 * 1000, acquired, VK_NULL_HANDLE, &presentImage);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
        {
            static int told = 0;
            if (told++ < 5)
                check(r, "vkAcquireNextImageKHR");
            return;
        }
        VkClearValue black{};
        black.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        bi.renderPass = rpPresent;
        bi.framebuffer = swapchainFramebuffers[presentImage];
        bi.renderArea = {{0, 0}, screen};
        bi.clearValueCount = 1;
        bi.pClearValues = &black;
        vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
        struct
        {
            int32_t mode, flip;
        } push{0, presentCpu ? 0 : 1};
        auto draw = [&](VkPipeline pipeline, Image &img, VkRect2D rect)
        {
            const Binding b[3] = {{img.view, linearClamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                  {img.view, linearClamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                  {img.view, linearClamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            const VkDescriptorSet set = imageSet(quadSetLayout, b, 3);
            if (!set)
                return;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, quadLayout, 0, 1, &set, 0, nullptr);
            const VkViewport vp{static_cast<float>(rect.offset.x), static_cast<float>(rect.offset.y), static_cast<float>(rect.extent.width),
                                static_cast<float>(rect.extent.height), 0.0f, 1.0f};
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &rect);
            vkCmdPushConstants(cmd, quadLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        };
        const float savedAspect = pictureAspect;
        if (presentCpu)
            pictureAspect = cpuAspect;
        draw(presentPicture, picture, fitRect());
        pictureAspect = savedAspect;
        push.mode = 1;
        push.flip = 0;
        draw(presentOverlay, overlay, VkRect2D{{0, 0}, screen});
        vkCmdEndRenderPass(cmd);
        boundPipeline = VK_NULL_HANDLE;
        lastQuadSet = VK_NULL_HANDLE;
        presentNext = true;
    }

    void Renderer::queuePresent()
    {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &drawn;
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &presentImage;
        const VkResult r = vkQueuePresentKHR(queue, &pi);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
        {
            static int told = 0;
            if (told++ < 5)
                check(r, "vkQueuePresentKHR");
        }
        lastPresentUs = native_renderer::nowUs();
    }

    // No picture of the renderer's for a while (the boot, a film, a load: the emulated GS draws): the host's picture
    // and overlay, presented when either is new (and every half second, so the screen keeps up).
    void Renderer::presentIdle()
    {
        lastIdleCheckUs = native_renderer::nowUs();
#if RC_VK_STATIC_ICD
        int w = 0, h = 0;
        const bool newPicture = rc_screen_picture_take(cpuStaging.mapped, static_cast<size_t>(cpuStaging.size), &w, &h, &cpuSerial) != 0;
        const bool newOverlay = rc_screen_overlay_serial() != overlaySeen;
        if (!newPicture && !newOverlay && lastIdleCheckUs - lastPresentUs < 500000u)
            return;
        if (!beginCommands())
            return;
        if (newPicture && w > 0 && h > 0)
        {
            if (!cpuPicture.image || cpuPicture.width != static_cast<uint32_t>(w) || cpuPicture.height != static_cast<uint32_t>(h))
            {
                lastQuadSet = VK_NULL_HANDLE;
                makeImage(cpuPicture, static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1, false,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            }
            if (cpuPicture.image)
            {
                barrier(cpuPicture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
                vkCmdCopyBufferToImage(cmd, cpuStaging.buffer, cpuPicture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            }
        }
        presentNext = false;
        // the renderer's last picture while the game still shows it (a pause in its frames: a load), else the host's
        presentCpu = !native_renderer::shown();
        if (presentCpu ? cpuPicture.image != VK_NULL_HANDLE : color.color.image != VK_NULL_HANDLE)
            recordPresent();
        presentCpu = false;
        if (submitAndWait() && presentNext)
            queuePresent();
        presentNext = false;
#endif
    }

    // Per-frame meshes no frame made for a while (the kept ones and the textures: VkBackend::evictKept).
    void Renderer::evictUnused()
    {
        static const bool texLog = std::getenv("SOTC_GL_TEX_LOG") != nullptr;
        if (texLog && arenaFrame % 300 == 0)
        {
            std::fprintf(stderr, "[vk:tex] frame %llu missing draws %llu uploads %llu textures %zu (%.1f MB)\n", (unsigned long long)arenaFrame,
                         (unsigned long long)texStats.missing, (unsigned long long)texStats.uploads, textures.size(), textureBytes / 1048576.0);
            texStats.missing = texStats.uploads = 0;
        }
        constexpr uint64_t kEvery = 256, kUnused = 1800;
        if (arenaFrame % kEvery != 0 || arenaFrame < kUnused)
            return;
        const uint64_t before = arenaFrame - kUnused;
        for (auto it = meshes.begin(); it != meshes.end();)
        {
            Mesh &m = it->second;
            if (m.dynamic && m.frame < before)
            {
                freeMesh(m);
                forget(m);
                it = meshes.erase(it);
            }
            else
                ++it;
        }
    }

    bool Renderer::uploadMesh(uint64_t id, const native_renderer::Vertex *vertices, uint32_t count)
    {
        Mesh &mesh = meshes[id];
        forget(mesh);
        freeMesh(mesh);
        mesh.used = arenaFrame;
        mesh.dynamic = mesh.indexed = mesh.skinned = false;
        mesh.count = 0;
        mesh.bytes = 0;
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(count) * sizeof(native_renderer::Vertex);
        if (!count)
            return true;
        mesh.vertices = allocate(meshPool, bytes, 16);
        if (!mesh.vertices || !blockOf(mesh.vertices).mapped)
            return false;
        std::memcpy(blockOf(mesh.vertices).mapped + mesh.vertices.offset, vertices, bytes);
        mesh.count = count;
        mesh.bytes = bytes;
        meshBytes += mesh.bytes;
        return true;
    }

    bool Renderer::uploadSkinned(uint64_t id, const native_renderer::SkinVertex *vertices, uint32_t count, const uint32_t *indices, uint32_t indexCount)
    {
        Mesh &mesh = meshes[id];
        forget(mesh);
        freeMesh(mesh);
        mesh.used = arenaFrame;
        mesh.dynamic = false;
        mesh.skinned = true;
        mesh.indexed = true;
        mesh.count = 0;
        mesh.bytes = 0;
        const VkDeviceSize vbytes = static_cast<VkDeviceSize>(count) * sizeof(native_renderer::SkinVertex), ibytes = static_cast<VkDeviceSize>(indexCount) * 4u;
        if (!count || !indexCount)
            return true;
        mesh.vertices = allocate(meshPool, vbytes, 16);
        mesh.indices = allocate(meshPool, ibytes, 16);
        if (!mesh.vertices || !mesh.indices)
        {
            freeMesh(mesh);
            return false;
        }
        std::memcpy(blockOf(mesh.vertices).mapped + mesh.vertices.offset, vertices, vbytes);
        std::memcpy(blockOf(mesh.indices).mapped + mesh.indices.offset, indices, ibytes);
        mesh.count = indexCount;
        mesh.bytes = vbytes + ibytes;
        meshBytes += mesh.bytes;
        skinnedBytes += mesh.bytes;
        return true;
    }

    // Uploads in the order asked for: meshes written into their memory at once (the GPU is idle), textures copied
    // from staging by this command buffer.
    void Renderer::applyUploadList(std::vector<Upload> &list)
    {
        VkDeviceSize stagingBytes = 0;
        for (const Upload &u : list)
            if (u.kind == Upload::TextureKind)
                stagingBytes += (u.bytes.size() + 15u) & ~VkDeviceSize(15u);
        if (stagingBytes && !ensureStream(staging, stagingBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
            stagingBytes = 0;
        VkDeviceSize at = 0;
        for (const Upload &u : list)
        {
            switch (u.kind)
            {
            case Upload::MeshKind:
                uploadMesh(u.id, reinterpret_cast<const native_renderer::Vertex *>(u.bytes.data()), u.count);
                break;
            case Upload::SkinnedKind:
                uploadSkinned(u.id, reinterpret_cast<const native_renderer::SkinVertex *>(u.bytes.data()), u.count, u.indices.data(),
                              static_cast<uint32_t>(u.indices.size()));
                break;
            case Upload::TextureKind:
            {
                if (!stagingBytes)
                    break;
                // Vulkan's mip chain ends at 1 x 1 (the levels past it are left out)
                uint32_t levels = 1;
                while (levels < std::max(1u, u.levels) && levels < 16u && ((u.width >> levels) || (u.height >> levels)))
                    ++levels;
                Texture &t = textures[u.id];
                forget(t);
                ++texStats.uploads;
                t.used = arenaFrame;
                t.bytes = u.bytes.size();
                if (!t.image.image || t.image.width != u.width || t.image.height != u.height || t.image.levels != levels)
                    makeImage(t.image, u.width, u.height, levels, false, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
                if (!t.image.image)
                {
                    textures.erase(u.id);
                    break;
                }
                textureBytes += t.bytes;
                std::memcpy(staging.mapped + at, u.bytes.data(), u.bytes.size());
                VkBufferImageCopy regions[16]{};
                uint32_t n = 0;
                VkDeviceSize offset = at;
                for (uint32_t level = 0, w = u.width, h = u.height; level < levels && level < 16u; ++level)
                {
                    regions[n].bufferOffset = offset;
                    regions[n].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
                    regions[n].imageExtent = {w, h, 1};
                    ++n;
                    offset += static_cast<VkDeviceSize>(w) * h * 4u;
                    w = std::max(1u, w / 2u);
                    h = std::max(1u, h / 2u);
                }
                endPass();
                t.image.layout = VK_IMAGE_LAYOUT_UNDEFINED; // the image is written whole
                barrier(t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                vkCmdCopyBufferToImage(cmd, staging.buffer, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, n, regions);
                barrier(t.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                at += (u.bytes.size() + 15u) & ~VkDeviceSize(15u);
                break;
            }
            }
        }
        list.clear();
    }

    // The frame's work before its draws, in the order the game thread asked for it: what eviction let go, the
    // uploads, the per-frame meshes.
    void Renderer::applyUploads()
    {
        for (Mesh &m : deadMeshes)
            freeMesh(m);
        for (Texture &t : deadTextures)
            destroyImage(t.image);
        deadMeshes.clear();
        deadTextures.clear();
        applyUploadList(uploads);
        for (const FrameMesh &f : frameMeshes)
        {
            Mesh &mesh = meshes[f.id];
            if (mesh.vertices || mesh.indices) // a kept mesh's id made again per frame
            {
                forget(mesh);
                freeMesh(mesh);
                mesh.bytes = 0;
            }
            mesh.dynamic = true;
            mesh.frame = arenaFrame;
            mesh.first = f.first;
            mesh.skinned = false;
            mesh.indexed = f.indexed;
            mesh.firstIndex = f.firstIndex;
            mesh.count = f.count;
        }
        frameMeshes.clear();
    }

    void Renderer::render(uint32_t w, uint32_t h)
    {
        ensureTargets(w, h);
        // The frame's per-frame vertices, indices and skin palettes (the GPU is idle: the streams may grow).
        ensureStream(vertexStream, std::max<VkDeviceSize>(arena.size(), 1) * sizeof(native_renderer::Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        ensureStream(indexStream, std::max<VkDeviceSize>(indexArena.size(), 1) * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        ensureStream(paletteStream, std::max<VkDeviceSize>(palettes.size(), 4) * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (!arena.empty())
            std::memcpy(vertexStream.mapped, arena.data(), arena.size() * sizeof(native_renderer::Vertex));
        if (!indexArena.empty())
            std::memcpy(indexStream.mapped, indexArena.data(), indexArena.size() * sizeof(uint32_t));
        if (!palettes.empty())
            std::memcpy(paletteStream.mapped, palettes.data(), palettes.size() * sizeof(float));
        frameSet = VK_NULL_HANDLE; // made with the palettes' stream as it is now

        drawOrder.clear();
        drawOrder.reserve(draws.size());
        for (size_t i = 0; i < draws.size(); ++i)
            drawOrder.push_back((static_cast<uint64_t>(draws[i].state.order) << 32) | i);
        std::sort(drawOrder.begin(), drawOrder.end());
        const bool fogOn = fog.enabled && fog.width > 0.0f;
        bool haveMirror = false;
        for (const Draw &d : draws)
            if (d.state.reflectionSource)
            {
                haveMirror = true;
                break;
            }
        static const bool hardShadows = std::getenv("SOTC_SHADOW_HARD") != nullptr;
        Image *waterSource = nullptr, *hazeSourceImage = nullptr;
        uint32_t softOrder = 0;
        float softAmount = 0.0f;
        if (!hardShadows)
            for (const Draw &d : draws)
                if (d.state.shadowVolume)
                {
                    softOrder = std::max(softOrder, d.state.order + 1u);
                    softAmount = d.state.shadow;
                }
        const bool softShadows = softOrder != 0;
        static const bool bgFog = std::getenv("SOTC_NATIVE_BGFOG") != nullptr;
        Binding none{dummy.view, linearClamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

        auto pass = [&](Target &targetT, const Fb &target, bool backgroundOnly, uint32_t fromOrder = 0, uint32_t toOrder = ~0u, bool load = false,
                        bool mirror = false, bool apart = false)
        {
            (void)targetT;
            if (apart) // a layer of its own over the picture's depth: colour cleared to nothing
            {
                const float nothing[4] = {0, 0, 0, 0};
                clear(target, nothing, false);
            }
            if (!load)
            {
                const float mirrorClear[4] = {fog.colour[0], fog.colour[1], fog.colour[2], 1.0f}, black[4] = {0, 0, 0, 1};
                clear(target, mirror ? mirrorClear : black, true); // reflectionBegin clears with alpha 0x80
            }
            if (!mirror && !backgroundOnly && !load && fogOn && native_renderer::tuning().skyImage)
            {
                // The background as the PS2 shows it: bgEnd's reduced copy, stretched back.
                bindTexture(0, backgroundSmall);
                quad(target, 0, QuadBlend::None);
            }
            uint32_t shadowOrder = 0;
            float shadowAmount = 0.0f;
            auto resolveShadows = [&]
            {
                if (!shadowAmount)
                    return;
                if (softShadows)
                {
                    shadowAmount = 0.0f;
                    return;
                }
                const float a[4] = {shadowAmount, 0, 0, 0};
                quad(target, 5, QuadBlend::Darken, a, nullptr, nullptr, false, true);
                shadowAmount = 0.0f;
            };
            for (const uint64_t key : drawOrder)
            {
                const Draw &d = draws[static_cast<size_t>(key & 0xFFFFFFFFu)];
                if (backgroundOnly && !d.state.background)
                    continue;
                if (mirror != d.state.reflectionSource)
                    continue;
                if (d.state.reflectionSample && !haveMirror)
                    continue;
                if (!backgroundOnly && fogOn && d.state.background)
                    continue;
                if (d.state.order < fromOrder || d.state.order >= toOrder)
                    continue;
                if (shadowAmount && d.state.order > shadowOrder)
                    resolveShadows();
                auto meshIt = meshes.find(d.mesh);
                if (meshIt == meshes.end() || meshIt->second.count == 0)
                    continue;
                Mesh &mesh = meshIt->second;
                mesh.used = arenaFrame;
                const bool skinned = mesh.skinned;
                if (skinned && (!skinOk || !d.state.skinPalette))
                    continue;
                uint32_t paletteBase = 0;
                if (skinned)
                {
                    paletteBase = d.state.skinPalette - 1u; // in qwords
                    const uint32_t count = std::min(d.state.skinPaletteCount, native_renderer::kSkinPaletteQwords);
                    if ((static_cast<size_t>(paletteBase) + count) * 4u > palettes.size())
                        continue;
                }
                if (d.state.shadowVolume && backgroundOnly)
                    continue;
                DrawUniforms u{};
                std::memcpy(u.clip, d.clip, sizeof(u.clip));
                std::memcpy(u.directions, d.state.light, sizeof(u.directions));
                std::memcpy(u.colours, d.state.light + 16, sizeof(u.colours));
                Binding tex[4] = {none, none, none, none};
                VkPipeline pipeline = VK_NULL_HANDLE;
                if (d.state.shadowVolume)
                {
                    u.misc[1] = 1.0f;
                    pipeline = mainPipeline(skinned, native_renderer::Blend::Normal, true, false, true);
                }
                else
                {
                    const bool backdrop = d.state.screen || d.state.background;
                    const bool depthTest = !(backdrop && !d.state.screenDepthTest);
                    const bool depthWrite = d.state.screenDepthTest ? false
                                            : backdrop             ? false
                                                                   : !(d.state.overlay || (d.state.blend == native_renderer::Blend::Additive && !d.state.writeDepth) || d.state.noDepthWrite);
                    pipeline = mainPipeline(skinned, d.state.blend, depthTest, depthWrite, false);
                    auto texIt = d.state.texture ? textures.find(d.state.texture) : textures.end();
                    if (d.state.texture && texIt == textures.end())
                        ++texStats.missing;
                    const int textured = d.state.previousFrame ? 5 : d.state.reflectionSample ? 2 : (d.state.refractionSample ? 3 : (d.state.screenCopy ? 4 : (texIt != textures.end() ? 1 : 0)));
                    if (textured == 5)
                        tex[0] = sampleTarget(&previous.color, target);
                    if (textured == 1)
                    {
                        Texture &t = texIt->second;
                        t.used = arenaFrame;
                        const uint32_t filter = d.state.screen ? 2u : static_cast<uint32_t>(std::clamp(native_renderer::tuning().textureFilter + 0.5f, 0.0f, 6.0f));
                        tex[0] = Binding{t.image.view, gameSampler(d.state.clamp & 3u, filter, t.image.levels > 1u), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                    }
                    if (!backgroundOnly)
                        tex[1] = sampleTarget(&backgroundSmall.color, target);
                    if (!mirror)
                    {
                        tex[2] = sampleTarget(&reflection.color, target);
                        Image *third = &refraction.color;
                        if (textured == 3 || textured == 4)
                        {
                            const native_renderer::Tuning &t = native_renderer::tuning();
                            const int source = static_cast<int>((textured == 4 ? t.hazeSource : t.waterSource) + 0.5f);
                            Image *redrawn = textured == 4 ? hazeSourceImage : waterSource;
                            third = redrawn && source != 2 ? redrawn : (source == 2 ? &previous.color : (source == 1 ? &snapshot.color : &refraction.color));
                        }
                        tex[3] = sampleTarget(third, target);
                    }
                    u.misc[0] = d.state.fresnel ? -1.0f : (d.state.lit ? 1.0f : 0.0f);
                    u.misc[1] = d.state.opacity;
                    u.misc[2] = d.state.uvOffset[0];
                    u.misc[3] = d.state.uvOffset[1];
                    const bool vertexFog = fog.vertexFog && !d.state.screen && (!d.state.background || bgFog);
                    u.fogA[0] = fog.start;
                    u.fogA[1] = fog.width;
                    u.fogA[2] = static_cast<float>(fog.steps);
                    u.fogA[3] = (!backgroundOnly && fogOn && d.state.fogged) ? 1.0f : 0.0f;
                    u.fogB[0] = static_cast<float>(width);
                    u.fogB[1] = static_cast<float>(height);
                    u.fogB[2] = vertexFog ? 1.0f : 0.0f;
                    u.fogB[3] = fog.base;
                    u.fogC[0] = fog.scale;
                    u.fogC[1] = d.state.reflectionSample ? d.state.mirrorRipple[0] : d.state.lodK;
                    u.fogC[2] = d.state.reflectionSample ? d.state.mirrorRipple[1] : d.state.lodScale;
                    u.fogC[3] = d.state.reflectionSample ? 0.0f : d.state.maxLod;
                    u.fogColour[0] = fog.colour[0];
                    u.fogColour[1] = fog.colour[1];
                    u.fogColour[2] = fog.colour[2];
                    u.fogColour[3] = static_cast<float>(textured);
                    u.debug[0] = static_cast<float>(d.state.debugColour & 0xFFu) / 255.0f;
                    u.debug[1] = static_cast<float>((d.state.debugColour >> 8) & 0xFFu) / 255.0f;
                    u.debug[2] = static_cast<float>((d.state.debugColour >> 16) & 0xFFu) / 255.0f;
                    u.debug[3] = d.state.debugColour ? 1.0f : 0.0f;
                    if (d.state.reflectionSample || d.state.refractionSample)
                    {
                        for (int k = 0; k < 3; ++k)
                            u.debug[k] = d.state.reflectionTint[k];
                        u.debug[3] = -d.state.reflectionTint[3];
                    }
                }
                if (!pipeline)
                    continue;
                beginPass(target); // after the textures: reading a target ends the render pass it was drawn in
                const uint32_t uniformOffset = pushUniforms(u);
                if (!frameSet)
                    continue;
                if (!lastTextureSet || !(tex[0] == lastTextures[0] && tex[1] == lastTextures[1] && tex[2] == lastTextures[2] && tex[3] == lastTextures[3]))
                {
                    lastTextureSet = imageSet(textureLayout, tex, 4);
                    for (int i = 0; i < 4; ++i)
                        lastTextures[i] = tex[i];
                }
                if (!lastTextureSet)
                    continue;
                bindPipeline(pipeline);
                const VkDescriptorSet sets[2] = {frameSet, lastTextureSet};
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mainLayout, 0, 2, sets, 1, &uniformOffset);
                if (skinned)
                    vkCmdPushConstants(cmd, mainLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(paletteBase), &paletteBase);
                if (mesh.dynamic)
                {
                    bindVertices(vertexStream.buffer, 0);
                    if (mesh.indexed)
                    {
                        bindIndices(indexStream.buffer, 0);
                        vkCmdDrawIndexed(cmd, mesh.count, 1, mesh.firstIndex, 0, 0);
                    }
                    else
                        vkCmdDraw(cmd, mesh.count, 1, mesh.first, 0);
                }
                else
                {
                    bindVertices(blockOf(mesh.vertices).buffer, mesh.vertices.offset);
                    if (mesh.indexed)
                    {
                        bindIndices(blockOf(mesh.indices).buffer, mesh.indices.offset);
                        vkCmdDrawIndexed(cmd, mesh.count, 1, 0, 0, 0);
                    }
                    else
                        vkCmdDraw(cmd, mesh.count, 1, 0, 0);
                }
                if (d.state.shadowVolume)
                {
                    shadowOrder = d.state.order;
                    shadowAmount = d.state.shadow;
                }
            }
            resolveShadows();
        };

        if (haveMirror)
            pass(reflection, fb(reflection), false, 0, ~0u, false, true);
        if (fogOn)
        {
            pass(background, fb(background), true);
            copy(background, backgroundSmall); // bgEnd: reduced to 256 x 256
        }
        const bool bloomOn = bloom.enabled;
        const bool dofOn = dof.enabled && dof.slices > 0;
        auto doBloom = [&]
        {
            const float a[4] = {bloom.threshold, bloom.zA, bloom.zB, 0};
            const float maskColour[4] = {bloom.maskColor[0], bloom.maskColor[1], bloom.maskColor[2], 1};
            bindTexture(2, color.depth);
            static const bool oldBloom = std::getenv("SOTC_BLOOM_OLD") != nullptr;
            if (!oldBloom)
            {
                // The GS chain, as the Metal renderer does it.
                quad(fb(gsBloom[0]), 15, QuadBlend::None, a, maskColour);
                bindTexture(0, gsBloom[0]);
                quad(fb(gsBloom[1]), 0, QuadBlend::None);
                bindTexture(0, gsBloom[1]);
                quad(fb(gsBloom[2]), 0, QuadBlend::None);
                const float alpha[4] = {gsBloomHistory ? bloom.temporal : 0.0f, 0, 0, 0};
                bindTexture(0, gsBloom[2]);
                bindTexture(1, gsBloom[4]);
                quad(fb(gsBloom[3]), 17, QuadBlend::None, alpha);
                const float r64[4] = {64, 0, 0, 0}, r128[4] = {128, 0, 0, 0};
                bindTexture(0, gsBloom[3]);
                quad(fb(gsBloom[4]), 16, QuadBlend::None, r64);
                gsBloomHistory = true;
                quad(fb(gsBloom[5]), 16, QuadBlend::None, r128);
                bindTexture(0, gsBloom[5]);
                quad(fb(gsBloom[6]), 0, QuadBlend::None);
                const float scale[4] = {static_cast<float>(width) / 512.0f, static_cast<float>(height) / 448.0f, 0, 0};
                const float colour[4] = {bloom.color[0], bloom.color[1], bloom.color[2], bloom.color[3]};
                bindTexture(0, gsBloom[6]);
                quad(colorPlain, 18, QuadBlend::Add, scale, colour);
                return;
            }
            quad(fb(bloomTex[0]), 8, QuadBlend::None, a, maskColour);
            const int rounds = std::clamp(static_cast<int>(native_renderer::tuning().bloomRounds + 0.5f), 1, 8);
            const float spreadGrow = native_renderer::tuning().bloomSpread;
            blur(rounds, spreadGrow);
            const float amount[4] = {bloomHistory ? 1.0f - bloom.temporal : 1.0f, 0, 0, 0};
            bindTexture(0, bloomTex[0]);
            bindTexture(1, bloomTex[2]);
            quad(fb(bloomTex[1]), 2, QuadBlend::None, amount);
            std::swap(bloomTex[1], bloomTex[2]);
            bloomHistory = true;
            const float colour[4] = {bloom.color[0], bloom.color[1], bloom.color[2], bloom.color[3]};
            bindTexture(0, bloomTex[2]);
            quad(colorPlain, 10, QuadBlend::Add, nullptr, colour);
        };
        auto doBlur = [&](const native_renderer::ScreenBlur &b, Target *redrawn)
        {
            if (b.steps <= 0)
                return;
            if (!redrawn)
                snapshotColor();
            bindTexture(0, redrawn ? *redrawn : snapshot);
            for (int i = 0; i < b.steps; ++i)
            {
                const float f = static_cast<float>(b.steps - i) / static_cast<float>(b.steps);
                const float params[4] = {(b.scale - 1.0f) * f + 1.0f, std::fabs(b.angle) >= 0.01570796f ? -b.angle * f : 0.0f, b.dx * f / 256.0f,
                                         -b.dy * f / 224.0f};
                const float alpha[4] = {1.0f / static_cast<float>(i + 1), 0, 0, 0};
                quad(fb(color), 3, QuadBlend::Over, alpha, nullptr, params, true);
            }
        };
        auto doGlow = [&]
        {
            pass(glowLayer, glowLayerFb, false, glow.order, glow.order + 1u, true, false, true);
            auto reduce = [&](Target &from, Target &to, float shift)
            {
                const float offset[4] = {shift / static_cast<float>(from.width), shift / static_cast<float>(from.height), 0, 0};
                bindTexture(0, from);
                quad(fb(to), 13, QuadBlend::None, offset);
            };
            reduce(glowLayer, glowSmall[0], 0.0f);
            if (glow.rounds > 0)
            {
                reduce(glowSmall[0], glowSmall[1], 0.0f);
                if (glow.rounds > 1)
                {
                    reduce(glowSmall[1], glowSmall[2], 0.0f);
                    if (glow.rounds > 2)
                        for (int i = 0; i <= glow.rounds; ++i)
                        {
                            reduce(glowSmall[2], glowSmall[3], 0.5f);
                            reduce(glowSmall[3], glowSmall[2], -0.5f);
                        }
                    reduce(glowSmall[2], glowSmall[1], 0.0f);
                }
                reduce(glowSmall[1], glowSmall[0], 0.0f);
            }
            const std::pair<Target *, float> layers[2] = {{&glowLayer, glow.amountA}, {&glowSmall[0], glow.amountB}};
            for (const auto &[source, amount] : layers)
            {
                if (!(amount > 0.0f))
                    continue;
                const float tint[4] = {glow.tint[0], glow.tint[1], glow.tint[2], amount};
                bindTexture(0, *source);
                quad(fb(color), 14, QuadBlend::Over, tint);
            }
        };
        auto doShadow = [&]
        {
            // The counted pixels as a mask (testing the main picture's stencil), reduced 4 x 4, blurred.
            const float nothing[4] = {0, 0, 0, 0};
            clear(shadowMaskFb, nothing, false);
            quad(shadowMaskFb, 6, QuadBlend::None, nullptr, nullptr, nullptr, false, true);
            bindTexture(0, shadowMask);
            quad(fb(bloomTex[0]), 7, QuadBlend::None);
            static const int rounds = std::getenv("SOTC_SHADOW_BLUR") ? std::atoi(std::getenv("SOTC_SHADOW_BLUR")) : 1;
            blur(rounds);
            const float amount[4] = {softAmount, 0, 0, 0};
            bindTexture(0, bloomTex[0]);
            quad(fb(color), 9, QuadBlend::Darken, amount);
        };
        bool haveRefraction = false;
        uint32_t screenCopyOrder = 0;
        for (const Draw &d : draws)
        {
            if (d.state.refractionSample)
                haveRefraction = true;
            if (d.state.screenCopy && !screenCopyOrder)
                screenCopyOrder = d.state.order;
        }
        auto doRefraction = [&]
        {
            const native_renderer::Tuning &t = native_renderer::tuning();
            const int haze = static_cast<int>(t.hazeSource + 0.5f), water = static_cast<int>(t.waterSource + 0.5f);
            if (haze == 2 && water == 2)
                return;
            snapshotColor();
            if (haze == 0 || water == 0)
            {
                copy(snapshot, refraction);
                copy(refraction, refractionHalf);
                copy(refractionHalf, refraction);
            }
        };
        auto doFog = [&]
        {
            const float a[4] = {fog.start, fog.width, static_cast<float>(fog.steps), 0};
            bindTexture(0, backgroundSmall);
            bindTexture(2, color.depth);
            quad(colorPlain, 12, QuadBlend::Over, a);
        };
        auto doDof = [&]
        {
            snapshotColor();
            copy(snapshot, refraction);
            for (int i = 0; i < dof.rounds; ++i)
            {
                copy(refraction, refractionHalf);
                copy(refractionHalf, refraction);
            }
            const float a[4] = {dof.zA, dof.zB, static_cast<float>(dof.focusZ), static_cast<float>(dof.slices)};
            bindTexture(0, refraction);
            bindTexture(2, color.depth);
            quad(colorPlain, 11, QuadBlend::Over, a);
        };
        struct Event
        {
            uint32_t order;
            int blur; // -1 bloom, -2 glow, -3 shadows, -4 refraction copy, -5 depth of field, -6 zFog
            int prefix = -1;
        };
        std::vector<Event> events;
        if (dofOn)
            events.push_back({dof.order, -5});
        if (fogOn && fog.steps > 0)
            events.push_back({fog.order, -6});
        if (softShadows)
            events.push_back({softOrder, -3});
        if (haveRefraction)
            events.push_back({9u, -4});
        if (screenCopyOrder)
            events.push_back({screenCopyOrder, -4});
        if (glow.enabled)
            events.push_back({glow.order, -2});
        if (bloomOn)
            events.push_back({bloom.order, -1});
        else
            bloomHistory = gsBloomHistory = false;
        for (size_t i = 0; i < blurs.size(); ++i)
            events.push_back({blurs[i].order, static_cast<int>(i)});
        std::stable_sort(events.begin(), events.end(), [](const Event &x, const Event &y) { return x.order < y.order; });
        static const bool eventLog = std::getenv("SOTC_GL_EVENT_LOG") != nullptr;
        if (eventLog)
        {
            std::string line;
            for (const Event &ev : events)
                line += " " + std::to_string(ev.order) + ":" + std::to_string(ev.blur);
            std::fprintf(stderr, "[vk:events] frame %llu draws %u%s\n", (unsigned long long)arenaFrame, static_cast<unsigned>(draws.size()), line.c_str());
        }
        const float redrawMode = native_renderer::tuning().midFrameRedraw;
        if (redrawMode >= 1.5f ? native_renderer::inEmulator() : redrawMode >= 0.5f)
        {
            const native_renderer::Tuning &t = native_renderer::tuning();
            const int hazeFrom = static_cast<int>(t.hazeSource + 0.5f), waterFrom = static_cast<int>(t.waterSource + 0.5f);
            int k = 0, last = -1;
            uint32_t lastOrder = ~0u;
            static const bool share = !std::getenv("SOTC_GL_REDRAW_NOSHARE");
            for (Event &ev : events)
            {
                const bool water = ev.blur == -4 && haveRefraction && ev.order == 9u && waterFrom != 2;
                const bool haze = ev.blur == -4 && screenCopyOrder && ev.order == screenCopyOrder && hazeFrom != 2;
                if (!(ev.blur >= 0 || water || haze))
                    continue;
                if (share && last >= 0 && ev.order == lastOrder)
                    ev.prefix = last;
                else
                {
                    if (k >= 3)
                        continue;
                    Target &pre = prefix[k];
                    if (!pre.color.image)
                        makeTarget(pre, w, h, true);
                    pass(pre, fb(pre), false, 0, ev.order, false);
                    ev.prefix = last = k++;
                    lastOrder = ev.order;
                }
                Target &pre = prefix[ev.prefix];
                auto small = [&](Target &to)
                {
                    copy(pre, to);
                    copy(to, refractionHalf);
                    copy(refractionHalf, to);
                };
                if (water)
                {
                    if (waterFrom == 0)
                        small(refraction);
                    waterSource = waterFrom == 0 ? &refraction.color : &pre.color;
                }
                if (haze)
                {
                    if (hazeFrom == 0)
                        small(hazeSmall);
                    hazeSourceImage = hazeFrom == 0 ? &hazeSmall.color : &pre.color;
                }
            }
        }
        uint32_t from = 0;
        bool loaded = false;
        for (const Event &ev : events)
        {
            if (ev.prefix >= 0 && ev.blur == -4)
                continue;
            pass(color, fb(color), false, from, ev.order, loaded);
            loaded = true;
            from = ev.order;
            if (ev.blur == -6)
                doFog();
            else if (ev.blur == -5)
                doDof();
            else if (ev.blur == -4)
                doRefraction();
            else if (ev.blur == -3)
                doShadow();
            else if (ev.blur == -2)
            {
                doGlow();
                from = glow.order + 1u; // drawn apart
            }
            else if (ev.blur < 0)
                doBloom();
            else
                doBlur(blurs[static_cast<size_t>(ev.blur)], ev.prefix >= 0 ? &prefix[ev.prefix] : nullptr);
        }
        pass(color, fb(color), false, from, ~0u, loaded);
        copy(color, fb(previous));
        if (direct)
            return; // shown by recordPresent
        // Read back (turned over by finishReadback: the picture is laid out as GL's, row 0 at the bottom).
        endPass();
        barrier(color.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cmd, color.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
        VkBufferMemoryBarrier hostRead{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        hostRead.srcQueueFamilyIndex = hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostRead.buffer = readback.buffer;
        hostRead.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hostRead, 0, nullptr);
        readbackWidth = w;
        readbackHeight = h;
    }

    void Renderer::finishReadback()
    {
        const uint32_t w = readbackWidth, h = readbackHeight;
        if (!w || !h)
            return;
        if (!readback.coherent)
        {
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = readback.memory;
            range.size = VK_WHOLE_SIZE;
            vkInvalidateMappedMemoryRanges(device, 1, &range);
        }
        std::lock_guard<std::mutex> lock(outputMutex);
        const size_t row = static_cast<size_t>(w) * 4u;
        output.resize(row * h);
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(output.data() + static_cast<size_t>(y) * row, readback.mapped + static_cast<size_t>(h - 1u - y) * row, row);
        outputWidth = w;
        outputHeight = h;
        hasOutput = true;
    }

    // The game thread's side of the renderer and the render thread: native_renderer_gl.cpp's GlBackend, the same
    // (the render thread makes every Vulkan call).
    class VkBackend final : public native_renderer::detail::Backend
    {
    public:
        explicit VkBackend(Renderer *renderer) : r(renderer)
        {
            const auto mb = [](const char *name, double fallback)
            {
                const char *v = std::getenv(name);
                return static_cast<uint64_t>((v ? std::atof(v) : fallback) * 1048576.0);
            };
            m_cacheBudget = mb("SOTC_GL_CACHE_MB", 0.0);
            if (!m_cacheBudget)
                m_cacheBudget = ~0ull;
            m_heapHigh = mb("SOTC_GL_HEAP_MB", 0.0);
            m_heapLimit = 0;
            m_flushBytes = mb("SOTC_GL_UPLOAD_FLUSH_MB", 64.0);
            m_memLog = std::getenv("SOTC_GL_MEM_LOG") || std::getenv("SOTC_GL_CACHE_LOG");
            m_startUs = native_renderer::nowUs();
            m_widescreen = std::getenv("RECOLOSSUS_WIDESCREEN") && std::atoi(std::getenv("RECOLOSSUS_WIDESCREEN")) != 0;
            if (const char *a = std::getenv("RECOLOSSUS_ASPECT"))
                m_forcedAspect = std::strcmp(a, "16:9") == 0 ? 169 : (std::strcmp(a, "4:3") == 0 ? 43 : (std::strcmp(a, "auto") == 0 ? -1 : 0));
            std::fprintf(stderr, "[native:vk] picture: %s%s\n", m_widescreen ? "widescreen 16:9 (RECOLOSSUS_WIDESCREEN)" : "4:3",
                         m_forcedAspect > 0 ? ", shown at RECOLOSSUS_ASPECT" : "");
            m_aspectLog = std::getenv("SOTC_FRAME_LOG") != nullptr;
            if (const char *v = std::getenv("RECOLOSSUS_RENDER_SCALE"))
                m_renderScale = std::strcmp(v, "game") == 0 ? 0.0f : std::clamp(static_cast<float>(std::atof(v)), 0.25f, 1.0f);
        }

        bool start()
        {
            std::thread([this] { loop(); }).detach();
            std::unique_lock<std::mutex> lock(m_mutex);
            while (!m_started)
                m_idle.wait_for(lock, kRecheck);
            return m_initOk;
        }

        bool hasMesh(uint64_t id) override { return m_knownMeshes.count(id) != 0; }
        void uploadMesh(uint64_t id, const native_renderer::Vertex *vertices, uint32_t count, bool perFrame) override
        {
            if (perFrame)
            {
                const uint32_t first = static_cast<uint32_t>(m_rec.arena.size());
                if (!m_rec.arena.append(vertices, count))
                {
                    ++m_skipped;
                    return;
                }
                m_rec.frameMeshes.push_back({id, first, count, 0u, false});
                return;
            }
            const auto *from = reinterpret_cast<const uint8_t *>(vertices);
            if (!queue(Renderer::Upload::MeshKind, id, count, from, count * sizeof(native_renderer::Vertex)))
                return;
            m_knownMeshes.insert(id);
            flushIfLarge();
        }
        native_renderer::Vertex *frameMesh(uint32_t count) override
        {
            m_pendingFirst = m_rec.arena.size();
            m_pendingIndexFirst = m_rec.indexArena.size();
            return m_rec.arena.grow(count);
        }
        uint32_t *frameMeshIndices(uint32_t count, uint32_t &base) override
        {
            base = static_cast<uint32_t>(m_pendingFirst);
            m_rec.indexArena.count = m_pendingIndexFirst;
            return m_rec.indexArena.grow(count);
        }
        void commitFrameMesh(uint64_t id, uint32_t count, uint32_t indexCount) override
        {
            m_rec.frameMeshes.push_back({id, static_cast<uint32_t>(m_pendingFirst), indexCount ? indexCount : count, static_cast<uint32_t>(m_pendingIndexFirst),
                                         indexCount != 0u});
            m_rec.arena.count = m_pendingFirst + count;
            m_rec.indexArena.count = m_pendingIndexFirst + indexCount;
        }
        void cancelFrameMesh() override
        {
            m_rec.arena.count = m_pendingFirst;
            m_rec.indexArena.count = m_pendingIndexFirst;
        }
        bool canSkin() override { return r->skinOk; }
        void uploadSkinnedMesh(uint64_t id, const native_renderer::SkinVertex *vertices, uint32_t count, const uint32_t *indices, uint32_t indexCount) override
        {
            const auto *from = reinterpret_cast<const uint8_t *>(vertices);
            Renderer::Upload *u = queue(Renderer::Upload::SkinnedKind, id, count, from, count * sizeof(native_renderer::SkinVertex), indexCount * 4u);
            if (!u)
                return;
            try
            {
                u->indices.assign(indices, indices + indexCount);
            }
            catch (const std::bad_alloc &)
            {
                m_recUploadBytes -= u->bytes.size() + indexCount * 4u;
                m_rec.uploads.pop_back();
                ++m_skipped;
                return;
            }
            m_knownMeshes.insert(id);
            flushIfLarge();
        }
        uint32_t framePalette(const float *qwords, uint32_t count) override
        {
            const size_t at = m_rec.palettes.size();
            m_rec.palettes.insert(m_rec.palettes.end(), qwords, qwords + count * 4u);
            return static_cast<uint32_t>(at / 4u) + 1u;
        }
        bool hasTexture(uint64_t id) override { return m_knownTextures.count(id) != 0; }
        void uploadTexture(uint64_t key, uint32_t width, uint32_t height, const uint8_t *rgba, uint32_t levels) override
        {
            if (!width || !height)
                return;
            levels = std::min(std::max(1u, levels), 16u);
            size_t bytes = 0;
            for (uint32_t level = 0, w = width, h = height; level < levels; ++level)
            {
                bytes += static_cast<size_t>(w) * h * 4u;
                w = std::max(1u, w / 2u);
                h = std::max(1u, h / 2u);
            }
            Renderer::Upload *u = queue(Renderer::Upload::TextureKind, key, 0u, rgba, bytes);
            if (!u)
                return;
            u->width = width;
            u->height = height;
            u->levels = levels;
            m_knownTextures.insert(key);
            flushIfLarge();
        }
        void beginFrame() override { clearFrame(); }
        void draw(uint64_t mesh, const float clip[16], const native_renderer::DrawState &state) override
        {
            // Perspective draws (w from z); flat ones (2D, the screen's own passes) have w = 1.
            const float wz = std::fabs(clip[3]) + std::fabs(clip[7]) + std::fabs(clip[11]);
            m_rec.draws.emplace_back(mesh, clip, state);
            if (wz < 1e-6f)
                return;
            if (m_widescreen)
            {
                // RECOLOSSUS_WIDESCREEN: the 3D world seen wider (hor+, 16:9): clip x times 3/4. The 2D (the HUD,
                // fades) is stretched to the screen; the game still culls models against its 4:3 view, so some pop
                // in at the sides.
                float *c = m_rec.draws.back().clip;
                c[0] *= 0.75f;
                c[4] *= 0.75f;
                c[8] *= 0.75f;
                c[12] *= 0.75f;
                clip = c;
            }
            // The aspect the game projects for, |clip row y| / |clip row x| (logged with SOTC_FRAME_LOG).
            const float x = clip[0] * clip[0] + clip[4] * clip[4] + clip[8] * clip[8];
            const float y = clip[1] * clip[1] + clip[5] * clip[5] + clip[9] * clip[9];
            if (x <= 0.0f || y <= 0.0f)
                return;
            const float ratio = std::sqrt(y / x);
            m_ratioSum += ratio;
            ++m_ratioCount;
            if (ratio > 1.25f && ratio < 1.42f)
                ++m_votes43;
            else if (ratio > 1.68f && ratio < 1.88f)
                ++m_votes169;
        }
        void setFog(const native_renderer::Fog &fog) override { m_rec.fog = fog; }
        void setBloom(const native_renderer::Bloom &bloom) override { m_rec.bloom = bloom; }
        void addScreenBlur(const native_renderer::ScreenBlur &blur) override { m_rec.blurs.push_back(blur); }
        void setGlowComposite(const native_renderer::GlowComposite &glow) override { m_rec.glow = glow; }
        void setDepthOfField(const native_renderer::DepthOfField &dof) override { m_rec.dof = dof; }
        void endFrame(uint32_t width, uint32_t height) override
        {
            const bool timed = native_renderer::tuning().frameLog;
            const uint64_t start = timed ? native_renderer::nowUs() : 0u;
            std::unique_lock<std::mutex> lock(m_mutex);
            waitIdle(lock);
            if (timed)
            {
                native_renderer::FrameCounters &c = native_renderer::frameCounters();
                c.renderUs += r->stats.renderUs;
                c.presentUs += r->stats.presentUs;
                c.targets += r->stats.targets;
            }
            r->stats = {};
            std::swap(r->draws, m_rec.draws);
            std::swap(r->arena, m_rec.arena);
            std::swap(r->indexArena, m_rec.indexArena);
            std::swap(r->palettes, m_rec.palettes);
            std::swap(r->blurs, m_rec.blurs);
            r->uploads.swap(m_rec.uploads);
            r->frameMeshes.swap(m_rec.frameMeshes);
            r->fog = m_rec.fog;
            r->bloom = m_rec.bloom;
            r->glow = m_rec.glow;
            r->dof = m_rec.dof;
            r->hold = m_rec.hold;
            m_rec.hold = false;
            clearFrame();
            m_rec.uploads.clear();
            m_uploadPeak = std::max(m_uploadPeak, m_frameUploadBytes + m_recUploadBytes);
            m_frameUploadBytes = m_recUploadBytes = 0;
            evictKept();
            trimRecording();
            if (m_memLog && native_renderer::nowUs() >= m_nextMemLog)
            {
                m_nextMemLog = native_renderer::nowUs() + 5000000u;
                logMemory();
            }
            decideAspect();
            if (r->direct && m_renderScale > 0.0f)
            {
                // The picture at the display's pixels: the rectangle it takes on the screen (RECOLOSSUS_RENDER_SCALE of it).
                const VkRect2D fit = r->fitRect();
                width = std::max(64u, static_cast<uint32_t>(fit.extent.width * m_renderScale + 0.5f)) & ~1u;
                height = std::max(64u, static_cast<uint32_t>(fit.extent.height * m_renderScale + 0.5f)) & ~1u;
            }
            m_width = width;
            m_height = height;
            m_queued = true;
            m_work.notify_one();
            const bool threaded = native_renderer::tuning().renderThread;
            if (!threaded)
                waitIdle(lock);
            if (timed)
            {
                native_renderer::FrameCounters &c = native_renderer::frameCounters();
                c.waitUs += native_renderer::nowUs() - start;
                c.renderThread = threaded;
            }
        }
        void finishFrames() override
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            waitIdle(lock);
        }
        bool timesItself() override { return true; }
        void holdFrame() override { m_rec.hold = true; }
        bool latestFrame(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height) override
        {
            std::lock_guard<std::mutex> lock(r->outputMutex);
            if (!r->hasOutput)
                return false;
            rgba = r->output;
            width = r->outputWidth;
            height = r->outputHeight;
            return true;
        }

    private:
        struct Recording
        {
            std::vector<Draw> draws;
            Renderer::Arena<native_renderer::Vertex> arena;
            Renderer::Arena<uint32_t> indexArena;
            std::vector<float> palettes;
            std::vector<native_renderer::ScreenBlur> blurs;
            native_renderer::Fog fog;
            native_renderer::Bloom bloom;
            native_renderer::GlowComposite glow;
            native_renderer::DepthOfField dof;
            bool hold = false;
            std::vector<Renderer::Upload> uploads;
            std::vector<Renderer::FrameMesh> frameMeshes;
        };

        // The aspect the picture is shown at: RECOLOSSUS_ASPECT=4:3 / 16:9, or the game's own (auto: the votes of 20
        // frames running, so a frame of the other kind does not flip it); the settings menu's Stretch fills the screen.
        void decideAspect()
        {
            const int mode = hostmenu::settings().screenMode.load(std::memory_order_relaxed);
            r->pictureStretched = mode == hostmenu::kScreenStretch;
            int vote = 0;
            if (m_votes169 >= 3 && m_votes169 > m_votes43)
                vote = 169;
            else if (m_votes43 >= 3 && m_votes43 > m_votes169)
                vote = 43;
            if (m_aspectLog && ++m_aspectFrames % 600u == 0u)
                std::fprintf(stderr, "[native:vk] aspect: shown %s, this frame %u draws at 4:3, %u at 16:9, mean ratio %.3f\n",
                             m_aspect == 169 ? "16:9" : "4:3", m_votes43, m_votes169, m_ratioCount ? m_ratioSum / m_ratioCount : 0.0f);
            m_votes43 = m_votes169 = m_ratioCount = 0;
            m_ratioSum = 0.0f;
            // The game draws for a 4:3 screen (it has no 16:9 setting): 4:3, or 16:9 with RECOLOSSUS_WIDESCREEN; the
            // projection's ratio is only logged (RECOLOSSUS_ASPECT=auto follows it, for a game patched to 16:9).
            int want = m_widescreen ? 169 : 43;
            if (m_forcedAspect > 0)
                want = m_forcedAspect;
            else if (m_forcedAspect < 0)
            {
                if (vote && vote != m_aspect && ++m_aspectRun >= 20u)
                    m_aspectRun = 0, want = vote;
                else
                {
                    if (!vote || vote == m_aspect)
                        m_aspectRun = 0;
                    want = m_aspect;
                }
            }
            if (want != m_aspect)
                std::fprintf(stderr, "[native:vk] aspect %s\n", want == 169 ? "16:9" : "4:3");
            m_aspect = want;
            r->pictureAspect = m_aspect == 169 ? 16.0f / 9.0f : 4.0f / 3.0f;
        }

        void clearFrame()
        {
            m_rec.draws.clear();
            m_rec.arena.clear();
            m_rec.indexArena.clear();
            m_rec.palettes.clear();
            m_rec.blurs.clear();
            m_rec.frameMeshes.clear();
        }

        Renderer::Upload *queue(Renderer::Upload::Kind kind, uint64_t id, uint32_t count, const uint8_t *data, size_t bytes, size_t extra = 0)
        {
            bool added = false;
            try
            {
                m_rec.uploads.emplace_back();
                added = true;
                Renderer::Upload &u = m_rec.uploads.back();
                u.kind = kind;
                u.id = id;
                u.count = count;
                u.bytes.assign(data, data + bytes);
            }
            catch (const std::bad_alloc &)
            {
                if (added)
                    m_rec.uploads.pop_back();
                ++m_skipped;
                return nullptr;
            }
            m_recUploadBytes += bytes + extra;
            return &m_rec.uploads.back();
        }

        void flushIfLarge()
        {
            if (m_recUploadBytes >= m_flushBytes)
                flushUploads();
        }
        void flushUploads()
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            while (m_sideBytes > 4u * m_flushBytes)
                m_idle.wait_for(lock, kRecheck);
            for (Renderer::Upload &u : m_rec.uploads)
                m_side.push_back(std::move(u));
            m_rec.uploads.clear();
            m_sideBytes += m_recUploadBytes;
            m_work.notify_one();
            m_frameUploadBytes += m_recUploadBytes;
            m_recUploadBytes = 0;
            ++m_flushes;
        }

        void trimRecording()
        {
            m_vertexPeak = std::max(m_vertexPeak, r->arena.size());
            m_indexPeak = std::max(m_indexPeak, r->indexArena.size());
            m_drawPeak = std::max(m_drawPeak, r->draws.size());
            if (++m_trimFrames >= 600u)
            {
                m_vertexPeakPrev = m_vertexPeak;
                m_indexPeakPrev = m_indexPeak;
                m_drawPeakPrev = m_drawPeak;
                m_vertexPeak = m_indexPeak = m_drawPeak = 0;
                m_trimFrames = 0;
            }
            const size_t vertices = std::max(m_vertexPeak, m_vertexPeakPrev), indices = std::max(m_indexPeak, m_indexPeakPrev),
                         draws = std::max(m_drawPeak, m_drawPeakPrev);
            if (m_rec.arena.capacity > vertices * 2u + 65536u)
                m_rec.arena.trim(vertices + vertices / 4u + 4096u);
            if (m_rec.indexArena.capacity > indices * 2u + 65536u)
                m_rec.indexArena.trim(indices + indices / 4u + 4096u);
            if (m_rec.draws.capacity() > draws * 2u + 1024u)
            {
                std::vector<Draw>().swap(m_rec.draws);
                try
                {
                    m_rec.draws.reserve(draws + draws / 4u + 256u);
                }
                catch (const std::bad_alloc &)
                {
                }
            }
        }

        void logMemory()
        {
            const uint64_t recent = r->arenaFrame > 120u ? r->arenaFrame - 120u : 0u;
            uint32_t kept = 0;
            uint64_t recentBytes = 0;
            for (const auto &m : r->meshes)
                if (!m.second.dynamic)
                {
                    ++kept;
                    recentBytes += m.second.used >= recent ? m.second.bytes : 0u;
                }
            for (const auto &t : r->textures)
                recentBytes += t.second.used >= recent ? t.second.bytes : 0u;
            const double MB = 1048576.0;
            const uint64_t arenas = r->arena.bytes() + r->indexArena.bytes() + m_rec.arena.bytes() + m_rec.indexArena.bytes();
            m_heap = heapUsed();
            std::fprintf(stderr,
                         "[vk:mem] t=%.0f heap %.1f MB | textures %u %.1f MB | meshes %u %.1f MB (skinned %.1f) | drawn in 120 frames %.1f MB | uploads peak %.1f MB a "
                         "frame, flushes %u | arenas %.1f MB | evicted aged %u, budget %u, pressure %u | skipped %u\n",
                         (native_renderer::nowUs() - m_startUs) / 1e6, m_heap / MB, static_cast<unsigned>(r->textures.size()), r->textureBytes / MB, kept,
                         r->meshBytes / MB, r->skinnedBytes / MB, recentBytes / MB, m_uploadPeak / MB, m_flushes, arenas / MB, m_evictedAged, m_evictedBudget,
                         m_evictedPressure, m_skipped);
            m_uploadPeak = 0;
            m_flushes = m_evictedAged = m_evictedBudget = m_evictedPressure = m_skipped = 0;
        }

        // What no frame drew for 1800 frames goes (ids hash content: another area's never come back), sooner past
        // the budget (SOTC_GL_CACHE_MB) or the heap mark (SOTC_GL_HEAP_MB): as the GL backend's evictKept.
        void evictKept()
        {
            constexpr uint64_t kEvery = 256, kUnused = 1800, kBudgetAge = 120, kPressureAge = 30, kHeapEvery = 16;
            const uint64_t frame = r->arenaFrame + 1u;
            if (m_heapHigh && frame % kHeapEvery == 0)
                m_heap = heapUsed();
            const bool aging = frame % kEvery == 0 && frame >= kUnused;
            const bool pressure = m_heapHigh && m_heap > m_heapHigh && frame >= m_nextPressure;
            const bool overBudget = r->textureBytes + r->meshBytes > m_cacheBudget && frame >= m_nextBudget;
            if (!aging && !pressure && !overBudget)
                return;
            m_keepMeshes.clear();
            m_keepTextures.clear();
            for (const Draw &d : r->draws)
            {
                m_keepMeshes.insert(d.mesh);
                if (d.state.texture)
                    m_keepTextures.insert(d.state.texture);
            }
            for (const Renderer::Upload &u : r->uploads)
                (u.kind == Renderer::Upload::TextureKind ? m_keepTextures : m_keepMeshes).insert(u.id);
            for (const Renderer::Upload &u : m_side)
                (u.kind == Renderer::Upload::TextureKind ? m_keepTextures : m_keepMeshes).insert(u.id);
            const uint64_t agedBefore = aging ? frame - kUnused : 0u;
            const uint64_t pressureBefore = pressure && frame > kPressureAge ? frame - kPressureAge : 0u;
            const uint64_t budgetBefore = overBudget && frame > kBudgetAge ? frame - kBudgetAge : 0u;
            const uint64_t oldest = std::max({agedBefore, pressureBefore, budgetBefore});
            m_candidates.clear();
            for (const auto &[id, m] : r->meshes)
                if (!m.dynamic && m.used < oldest && !m_keepMeshes.count(id))
                    m_candidates.push_back({m.used, id, m.bytes, false});
            for (const auto &[id, t] : r->textures)
                if (t.used < oldest && !m_keepTextures.count(id))
                    m_candidates.push_back({t.used, id, t.bytes, true});
            std::sort(m_candidates.begin(), m_candidates.end(), [](const Candidate &a, const Candidate &b) { return a.used < b.used; });
            uint64_t cached = r->textureBytes + r->meshBytes;
            const uint64_t target = m_cacheBudget - m_cacheBudget / 8u;
            for (const Candidate &c : m_candidates)
            {
                const bool aged = c.used < agedBefore, pressed = c.used < pressureBefore, budget = c.used < budgetBefore && cached > target;
                if (!aged && !pressed && !budget)
                    continue;
                if (aged)
                    ++m_evictedAged;
                else if (pressed)
                    ++m_evictedPressure;
                else
                    ++m_evictedBudget;
                cached -= c.bytes;
                if (c.texture)
                {
                    auto it = r->textures.find(c.id);
                    r->forget(it->second);
                    r->deadTextures.push_back(it->second);
                    r->textures.erase(it);
                    m_knownTextures.erase(c.id);
                }
                else
                {
                    auto it = r->meshes.find(c.id);
                    r->forget(it->second);
                    r->deadMeshes.push_back(it->second);
                    r->meshes.erase(it);
                    m_knownMeshes.erase(c.id);
                }
            }
            if (overBudget && cached > m_cacheBudget)
                m_nextBudget = frame + 64u;
            if (pressure)
            {
                m_nextPressure = frame + kHeapEvery;
                m_rec.arena.trim(0);
                m_rec.indexArena.trim(0);
            }
        }

        void loop()
        {
            ThreadNaming::SetCurrentThreadName("VkRenderThread");
            const bool ok = r->init();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_started = true;
                m_initOk = ok;
            }
            m_idle.notify_all();
            if (!ok)
                return;
            for (;;)
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                bool idle = false;
                while (!m_queued && m_side.empty())
                {
                    if (r->idleDue())
                    {
                        idle = true;
                        break;
                    }
                    m_work.wait_for(lock, kRecheck);
                }
                if (idle)
                {
                    m_busy = true;
                    lock.unlock();
                    r->presentIdle();
                    lock.lock();
                    m_busy = false;
                    m_idle.notify_all();
                    continue;
                }
                if (!m_side.empty())
                {
                    m_sideApplying.swap(m_side);
                    m_sideBytes = 0;
                    m_busy = true;
                    lock.unlock();
                    r->uploadsAlone(m_sideApplying);
                    lock.lock();
                    if (!m_queued)
                    {
                        m_busy = false;
                        m_idle.notify_all();
                        continue;
                    }
                }
                m_queued = false;
                m_busy = true;
                const uint32_t width = m_width, height = m_height;
                lock.unlock();
                {
                    const uint64_t start = native_renderer::nowUs();
                    ++r->arenaFrame;
                    r->frame(width, height);
                    r->evictUnused();
                    r->stats.renderUs += native_renderer::nowUs() - start;
                }
                lock.lock();
                m_busy = false;
                m_idle.notify_all();
            }
        }

        Renderer *r;
        Recording m_rec;
        native_renderer::IdSet m_knownMeshes, m_knownTextures;
        size_t m_pendingFirst = 0, m_pendingIndexFirst = 0;
        struct Candidate
        {
            uint64_t used, id, bytes;
            bool texture;
        };
        std::vector<Candidate> m_candidates;
        std::unordered_set<uint64_t> m_keepMeshes, m_keepTextures;
        uint64_t m_cacheBudget = ~0ull, m_heapHigh = 0, m_heapLimit = 0, m_flushBytes = 0, m_heap = 0;
        uint64_t m_nextBudget = 0, m_nextPressure = 0;
        uint64_t m_recUploadBytes = 0, m_frameUploadBytes = 0, m_uploadPeak = 0;
        size_t m_vertexPeak = 0, m_indexPeak = 0, m_drawPeak = 0, m_vertexPeakPrev = 0, m_indexPeakPrev = 0, m_drawPeakPrev = 0;
        uint32_t m_trimFrames = 0;
        bool m_memLog = false;
        uint64_t m_startUs = 0, m_nextMemLog = 0;
        uint32_t m_flushes = 0, m_evictedAged = 0, m_evictedBudget = 0, m_evictedPressure = 0, m_skipped = 0;
        std::mutex m_mutex;
        std::condition_variable m_work, m_idle;
        static constexpr std::chrono::milliseconds kRecheck{2};
        void waitIdle(std::unique_lock<std::mutex> &lock)
        {
            while (m_queued || m_busy)
                m_idle.wait_for(lock, kRecheck);
        }
        bool m_started = false, m_initOk = false;
        bool m_queued = false;
        bool m_busy = false;
        std::vector<Renderer::Upload> m_side, m_sideApplying;
        uint64_t m_sideBytes = 0;
        uint32_t m_width = 0, m_height = 0;
        // the aspect (decideAspect) and the picture's size on the display
        uint32_t m_votes43 = 0, m_votes169 = 0, m_ratioCount = 0, m_aspectRun = 0, m_aspectFrames = 0;
        float m_ratioSum = 0.0f;
        int m_aspect = 43, m_forcedAspect = 0;
        bool m_aspectLog = false, m_widescreen = false;
        float m_renderScale = 1.0f;
    };
}

namespace native_renderer::detail
{
    // reColossus: the PS5's backend (native_renderer.cpp picks it there, patches/celosia-ps5.patch).
    Backend *createVkBackend()
    {
        auto *renderer = new Renderer();
        auto *backend = new VkBackend(renderer);
        if (!backend->start())
        {
            std::fprintf(stderr, "[native:vk] the Vulkan renderer could not start: the emulated GS draws the game\n");
            return nullptr; // its thread has ended
        }
        return backend;
    }
}
