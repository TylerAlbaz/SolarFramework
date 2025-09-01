// renderer_api.cpp  — ABI v3 implementation
#include "renderer_api.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <windows.h>
#include <vulkan/vulkan.h>

#include <vector>
#include <string>
#include <cstring>
#include <cstdint>
#include <atomic>

//========== SPIR-V blobs produced by your shader build ==========
static const uint32_t VS_SPV[] = {
#   include "shaders/vs_ndc_passthrough.spv.inc"   // passes position to clip space
};
static const uint32_t FS_SPV[] = {
#   include "shaders/fs_solid_color.spv.inc"       // solid color via push constants
};
static_assert(sizeof(VS_SPV) % 4 == 0, "VS_SPV misaligned");
static_assert(sizeof(FS_SPV) % 4 == 0, "FS_SPV misaligned");

//========== global API + logging ==========
static thread_local std::string g_last_error;
static const char* get_last_error_cstr() { return g_last_error.c_str(); }

static fm_renderer_api g_api{}; // will be filled by fmGetRendererAPI

static void log_msg(int level, const char* msg) {
    if (g_api.hdr.log) {
        g_api.hdr.log(level, msg, g_api.hdr.log_user);
    }
}

//========== helpers ==========
static uint32_t find_memtype(VkPhysicalDevice phys, uint32_t type_bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    }
    return UINT32_MAX;
}

static bool pick_graphics_family(VkPhysicalDevice pd, uint32_t& family) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, props.data());
    for (uint32_t i = 0; i < count; ++i)
        if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = i; return true; }
    return false;
}

static bool supports_present(VkPhysicalDevice pd, uint32_t family, VkSurfaceKHR surface) {
    VkBool32 sup = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(pd, family, surface, &sup);
    return sup == VK_TRUE;
}

static VkSurfaceFormatKHR choose_surface_format(const std::vector<VkSurfaceFormatKHR>& formats) {
    for (auto& f : formats) {
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_SRGB) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return f;
    }
    return formats[0];
}
static VkPresentModeKHR choose_present_mode(const std::vector<VkPresentModeKHR>& modes, bool vsync) {
    if (vsync) return VK_PRESENT_MODE_FIFO_KHR; // guaranteed available
    for (auto m : modes) if (m == VK_PRESENT_MODE_MAILBOX_KHR)   return m;
    for (auto m : modes) if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) return m;
    return VK_PRESENT_MODE_FIFO_KHR;
}
static inline VkExtent2D client_extent(HWND hwnd) {
    RECT rc{}; GetClientRect(hwnd, &rc);
    return VkExtent2D{ (uint32_t)(rc.right - rc.left), (uint32_t)(rc.bottom - rc.top) };
}

//========== device state ==========
struct Device {
    // window
    HWND                hwnd = nullptr;

    // vulkan core
    VkInstance          instance = VK_NULL_HANDLE;
    VkPhysicalDevice    phys = VK_NULL_HANDLE;
    VkDevice            device = VK_NULL_HANDLE;
    uint32_t            gfxFam = 0xFFFFFFFF;
    VkQueue             gfxQ = VK_NULL_HANDLE;

    // surface/swapchain
    VkSurfaceKHR        surface = VK_NULL_HANDLE;
    VkSwapchainKHR      swap = VK_NULL_HANDLE;
    VkFormat            swapFmt = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D          extent{ 0,0 };
    std::vector<VkImage>      images;
    std::vector<VkImageView>  views;

    // render pass + framebuffers
    VkRenderPass                 rp = VK_NULL_HANDLE;
    std::vector<VkFramebuffer>   fbs;

    // commands + sync
    VkCommandPool                cmdPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cbs;
    VkSemaphore                  semAcquire = VK_NULL_HANDLE;
    VkSemaphore                  semRender = VK_NULL_HANDLE;
    VkFence                      fence = VK_NULL_HANDLE;
    uint32_t                     curImg = 0;

    // pipeline (simple lines)
    VkPipelineLayout             layout = VK_NULL_HANDLE;
    VkPipeline                   pipe = VK_NULL_HANDLE;

    // CPU-visible vertex buffer (reused each frame)
    VkBuffer                     vbuf = VK_NULL_HANDLE;
    VkDeviceMemory               vmem = VK_NULL_HANDLE;
    size_t                       vcap = 0;
    size_t                       vused = 0;
    void* mapped = nullptr;

    // state for this frame
    float                        color[4]{ 0.85f, 0.85f, 0.85f, 1.0f };
    uint32_t                     verts_this_frame = 0;
    bool                         pending_draw = false;

    // resize + matrices (doubles kept for future)
    std::atomic<bool>            needs_recreate{ false };
    double                       view3x4[12]{};
    double                       proj4x4[16]{};
    double                       origin3[3]{};
    bool                         vsync = false;
};

// handle helpers
static inline Device* H2D(fm_handle h) { return reinterpret_cast<Device*>(static_cast<uintptr_t>(h)); }
static inline fm_handle D2H(Device* p) { return static_cast<fm_handle>(reinterpret_cast<uintptr_t>(p)); }

//========== creation helpers ==========
static bool create_render_pass(Device* d) {
    VkAttachmentDescription color{};
    color.format = d->swapFmt;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference cref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &cref;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpci.attachmentCount = 1; rpci.pAttachments = &color;
    rpci.subpassCount = 1; rpci.pSubpasses = &sub;
    rpci.dependencyCount = 1; rpci.pDependencies = &dep;

    return vkCreateRenderPass(d->device, &rpci, nullptr, &d->rp) == VK_SUCCESS;
}

static bool create_lines_pipeline(Device* d) {
    // push constants: fragment color
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0; pcr.size = sizeof(float) * 4;

    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(d->device, &plci, nullptr, &d->layout) != VK_SUCCESS) return false;

    VkShaderModuleCreateInfo smci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smci.codeSize = sizeof(VS_SPV); smci.pCode = VS_SPV;
    VkShaderModule vs = VK_NULL_HANDLE;
    if (vkCreateShaderModule(d->device, &smci, nullptr, &vs) != VK_SUCCESS) return false;

    smci.codeSize = sizeof(FS_SPV); smci.pCode = FS_SPV;
    VkShaderModule fs = VK_NULL_HANDLE;
    if (vkCreateShaderModule(d->device, &smci, nullptr, &fs) != VK_SUCCESS) {
        vkDestroyShaderModule(d->device, vs, nullptr); return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs; stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

    // float3 positions
    VkVertexInputBindingDescription bind{};
    bind.binding = 0; bind.stride = sizeof(float) * 3; bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attr{};
    attr.location = 0; attr.binding = 0; attr.format = VK_FORMAT_R32G32B32_SFLOAT; attr.offset = 0;

    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 1; vi.pVertexAttributeDescriptions = &attr;

    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;

    VkPipelineViewportStateCreateInfo vpci{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vpci.viewportCount = 1; vpci.scissorCount = 1;

    VkDynamicState dynStates[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH };
    VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = 3; dyn.pDynamicStates = dynStates;

    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;

    VkGraphicsPipelineCreateInfo gp{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.stageCount = 2; gp.pStages = stages;
    gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vpci;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pColorBlendState = &cb;
    gp.pDynamicState = &dyn;
    gp.layout = d->layout; gp.renderPass = d->rp; gp.subpass = 0;

    VkResult pr = vkCreateGraphicsPipelines(d->device, VK_NULL_HANDLE, 1, &gp, nullptr, &d->pipe);
    vkDestroyShaderModule(d->device, vs, nullptr);
    vkDestroyShaderModule(d->device, fs, nullptr);
    return pr == VK_SUCCESS;
}

static bool create_vertex_buffer(Device* d, size_t min_bytes) {
    if (min_bytes < (1u << 16)) min_bytes = (1u << 16);

    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = min_bytes;
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d->device, &bi, nullptr, &d->vbuf) != VK_SUCCESS) return false;

    VkMemoryRequirements mr{}; vkGetBufferMemoryRequirements(d->device, d->vbuf, &mr);
    uint32_t type = find_memtype(d->phys, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) return false;

    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size; mai.memoryTypeIndex = type;
    if (vkAllocateMemory(d->device, &mai, nullptr, &d->vmem) != VK_SUCCESS) return false;
    if (vkBindBufferMemory(d->device, d->vbuf, d->vmem, 0) != VK_SUCCESS) return false;
    if (vkMapMemory(d->device, d->vmem, 0, mr.size, 0, &d->mapped) != VK_SUCCESS) return false;

    d->vcap = mr.size; d->vused = 0;
    return true;
}

static void destroy_swapchain_objects(Device* d) {
    for (auto fb : d->fbs) if (fb) vkDestroyFramebuffer(d->device, fb, nullptr);
    d->fbs.clear();
    for (auto v : d->views) if (v) vkDestroyImageView(d->device, v, nullptr);
    d->views.clear();
    d->images.clear();

    if (!d->cbs.empty()) {
        vkFreeCommandBuffers(d->device, d->cmdPool, (uint32_t)d->cbs.size(), d->cbs.data());
        d->cbs.clear();
    }
    if (d->swap) {
        vkDestroySwapchainKHR(d->device, d->swap, nullptr);
        d->swap = VK_NULL_HANDLE;
    }
}

static bool create_swapchain_objects(Device* d) {
    // Query current size; delay if minimized
    VkExtent2D ex = client_extent(d->hwnd);
    if (ex.width == 0 || ex.height == 0) return false;

    // Capabilities
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d->phys, d->surface, &caps);

    uint32_t pmCount = 0, fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(d->phys, d->surface, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(d->phys, d->surface, &fmtCount, formats.data());
    vkGetPhysicalDeviceSurfacePresentModesKHR(d->phys, d->surface, &pmCount, nullptr);
    std::vector<VkPresentModeKHR> modes(pmCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(d->phys, d->surface, &pmCount, modes.data());

    // Choose format/present mode
    VkSurfaceFormatKHR sf = choose_surface_format(formats);
    d->swapFmt = sf.format;
    VkPresentModeKHR   pm = choose_present_mode(modes, d->vsync);

    uint32_t imgCount = caps.minImageCount + 1;
    if (caps.maxImageCount && imgCount > caps.maxImageCount) imgCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sci.surface = d->surface;
    sci.minImageCount = imgCount;
    sci.imageFormat = sf.format;
    sci.imageColorSpace = sf.colorSpace;
    sci.imageExtent = ex;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = pm;
    sci.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(d->device, &sci, nullptr, &d->swap) != VK_SUCCESS) {
        g_last_error = "vkCreateSwapchainKHR failed";
        return false;
    }

    d->extent = ex;

    uint32_t ic = 0; vkGetSwapchainImagesKHR(d->device, d->swap, &ic, nullptr);
    d->images.resize(ic); vkGetSwapchainImagesKHR(d->device, d->swap, &ic, d->images.data());
    d->views.resize(ic);
    for (uint32_t i = 0; i < ic; ++i) {
        VkImageViewCreateInfo iv{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        iv.image = d->images[i];
        iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv.format = d->swapFmt;
        iv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        iv.subresourceRange.levelCount = 1; iv.subresourceRange.layerCount = 1;
        if (vkCreateImageView(d->device, &iv, nullptr, &d->views[i]) != VK_SUCCESS) {
            g_last_error = "vkCreateImageView failed";
            return false;
        }
    }

    // Create framebuffers
    d->fbs.resize(ic);
    for (uint32_t i = 0; i < ic; ++i) {
        VkImageView att[]{ d->views[i] };
        VkFramebufferCreateInfo fbci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fbci.renderPass = d->rp; fbci.attachmentCount = 1; fbci.pAttachments = att;
        fbci.width = d->extent.width; fbci.height = d->extent.height; fbci.layers = 1;
        if (vkCreateFramebuffer(d->device, &fbci, nullptr, &d->fbs[i]) != VK_SUCCESS) {
            g_last_error = "vkCreateFramebuffer failed";
            return false;
        }
    }

    // Allocate command buffers (one per backbuffer)
    d->cbs.resize(ic);
    VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = d->cmdPool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = ic;
    if (vkAllocateCommandBuffers(d->device, &cbai, d->cbs.data()) != VK_SUCCESS) {
        g_last_error = "vkAllocateCommandBuffers failed";
        return false;
    }
    return true;
}

static bool recreate_swapchain(Device* d) {
    VkExtent2D ce = client_extent(d->hwnd);
    if (ce.width == 0 || ce.height == 0) return false;

    vkDeviceWaitIdle(d->device);
    destroy_swapchain_objects(d);
    if (!create_swapchain_objects(d)) return false;

    log_msg(1, "Vulkan: Swapchain recreated.");
    d->needs_recreate.store(false, std::memory_order_release);
    return true;
}

//========== API function impls ==========
static void FM_CALL set_logger_impl(fm_log_fn cb, void* user) {
    g_api.hdr.log = cb; g_api.hdr.log_user = user;
    if (cb) cb(1, "Logger installed (ABI v3).", user);
}

static int FM_CALL create_device_impl(const fm_renderer_desc* desc, fm_handle* out_dev) {
    if (!out_dev || !desc || !desc->hwnd) { g_last_error = "bad args"; return FM_E_BADARGS; }
    *out_dev = 0;

    Device* d = new Device();
    d->hwnd = (HWND)desc->hwnd;
    d->vsync = (desc->vsync != 0);

    // Instance
    const char* instExts[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "SolarFramework"; app.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = (uint32_t)(sizeof(instExts) / sizeof(instExts[0]));
    ici.ppEnabledExtensionNames = instExts;

    if (vkCreateInstance(&ici, nullptr, &d->instance) != VK_SUCCESS) { g_last_error = "vkCreateInstance failed"; delete d; return FM_E_DEVICE; }
    log_msg(1, "Vulkan: Instance created.");

    // Surface
    VkWin32SurfaceCreateInfoKHR sci{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    sci.hinstance = (HINSTANCE)GetModuleHandleW(nullptr);
    sci.hwnd = d->hwnd;
    if (vkCreateWin32SurfaceKHR(d->instance, &sci, nullptr, &d->surface) != VK_SUCCESS) {
        g_last_error = "vkCreateWin32SurfaceKHR failed"; vkDestroyInstance(d->instance, nullptr); delete d; return FM_E_DEVICE;
    }

    // Physical device + queue
    uint32_t n = 0; vkEnumeratePhysicalDevices(d->instance, &n, nullptr);
    if (!n) { g_last_error = "No GPUs"; vkDestroySurfaceKHR(d->instance, d->surface, nullptr); vkDestroyInstance(d->instance, nullptr); delete d; return FM_E_DEVICE; }
    std::vector<VkPhysicalDevice> devs(n); vkEnumeratePhysicalDevices(d->instance, &n, devs.data());

    for (auto pd : devs) {
        uint32_t fam;
        if (pick_graphics_family(pd, fam) && supports_present(pd, fam, d->surface)) {
            d->phys = pd; d->gfxFam = fam; break;
        }
    }
    if (!d->phys) { g_last_error = "No device with graphics+present"; vkDestroySurfaceKHR(d->instance, d->surface, nullptr); vkDestroyInstance(d->instance, nullptr); delete d; return FM_E_DEVICE; }

    // Logical device
    float prio = 1.f;
    VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = d->gfxFam; qci.queueCount = 1; qci.pQueuePriorities = &prio;

    const char* devExts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)(sizeof(devExts) / sizeof(devExts[0]));
    dci.ppEnabledExtensionNames = devExts;

    if (vkCreateDevice(d->phys, &dci, nullptr, &d->device) != VK_SUCCESS) {
        g_last_error = "vkCreateDevice failed"; vkDestroySurfaceKHR(d->instance, d->surface, nullptr); vkDestroyInstance(d->instance, nullptr); delete d; return FM_E_DEVICE;
    }
    vkGetDeviceQueue(d->device, d->gfxFam, 0, &d->gfxQ);

    // Command pool & sync
    VkCommandPoolCreateInfo cpci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpci.queueFamilyIndex = d->gfxFam;
    vkCreateCommandPool(d->device, &cpci, nullptr, &d->cmdPool);

    VkSemaphoreCreateInfo seci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateSemaphore(d->device, &seci, nullptr, &d->semAcquire);
    vkCreateSemaphore(d->device, &seci, nullptr, &d->semRender);
    vkCreateFence(d->device, &fci, nullptr, &d->fence);

    // Render pass will be created after swapchain chooses the format, so:
    // 1) create swapchain (+ views, cbs)
    if (!create_swapchain_objects(d)) {
        g_last_error = "swapchain create failed";
        vkDestroyFence(d->device, d->fence, nullptr);
        vkDestroySemaphore(d->device, d->semRender, nullptr);
        vkDestroySemaphore(d->device, d->semAcquire, nullptr);
        vkDestroyCommandPool(d->device, d->cmdPool, nullptr);
        vkDestroyDevice(d->device, nullptr);
        vkDestroySurfaceKHR(d->instance, d->surface, nullptr);
        vkDestroyInstance(d->instance, nullptr);
        delete d; return FM_E_DEVICE;
    }

    // 2) create render pass, then rebuild framebuffers to use it
    if (!create_render_pass(d)) {
        g_last_error = "vkCreateRenderPass failed";
        destroy_swapchain_objects(d);
        vkDestroyFence(d->device, d->fence, nullptr);
        vkDestroySemaphore(d->device, d->semRender, nullptr);
        vkDestroySemaphore(d->device, d->semAcquire, nullptr);
        vkDestroyCommandPool(d->device, d->cmdPool, nullptr);
        vkDestroyDevice(d->device, nullptr);
        vkDestroySurfaceKHR(d->instance, d->surface, nullptr);
        vkDestroyInstance(d->instance, nullptr);
        delete d; return FM_E_DEVICE;
    }
    // rebuild FBs now with rp
    for (auto fb : d->fbs) if (fb) vkDestroyFramebuffer(d->device, fb, nullptr);
    d->fbs.clear();
    d->fbs.resize(d->views.size());
    for (uint32_t i = 0; i < d->views.size(); ++i) {
        VkImageView att[]{ d->views[i] };
        VkFramebufferCreateInfo fbci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fbci.renderPass = d->rp; fbci.attachmentCount = 1; fbci.pAttachments = att;
        fbci.width = d->extent.width; fbci.height = d->extent.height; fbci.layers = 1;
        if (vkCreateFramebuffer(d->device, &fbci, nullptr, &d->fbs[i]) != VK_SUCCESS) {
            g_last_error = "vkCreateFramebuffer (initial) failed";
            // continue; we'll fail draw if needed
        }
    }

    // 3) pipeline + vertex buffer
    if (!create_lines_pipeline(d) || !create_vertex_buffer(d, size_t{ 1 } << 20)) {
        g_last_error = "pipeline or vertex buffer creation failed";
        // continue; but drawing will fail
    }

    *out_dev = D2H(d);
    log_msg(1, "Vulkan: swapchain + lines pipeline ready.");
    return FM_OK;
}

static void FM_CALL destroy_device_impl(fm_handle h) {
    Device* d = H2D(h); if (!d) return;
    vkDeviceWaitIdle(d->device);

    if (d->mapped) vkUnmapMemory(d->device, d->vmem);
    if (d->vbuf)   vkDestroyBuffer(d->device, d->vbuf, nullptr);
    if (d->vmem)   vkFreeMemory(d->device, d->vmem, nullptr);

    if (d->pipe)   vkDestroyPipeline(d->device, d->pipe, nullptr);
    if (d->layout) vkDestroyPipelineLayout(d->device, d->layout, nullptr);

    destroy_swapchain_objects(d);
    if (d->rp)     vkDestroyRenderPass(d->device, d->rp, nullptr);

    if (d->fence)      vkDestroyFence(d->device, d->fence, nullptr);
    if (d->semRender)  vkDestroySemaphore(d->device, d->semRender, nullptr);
    if (d->semAcquire) vkDestroySemaphore(d->device, d->semAcquire, nullptr);

    if (d->cmdPool) vkDestroyCommandPool(d->device, d->cmdPool, nullptr);
    if (d->surface) vkDestroySurfaceKHR(d->instance, d->surface, nullptr);
    if (d->device)  vkDestroyDevice(d->device, nullptr);
    if (d->instance)vkDestroyInstance(d->instance, nullptr);

    delete d;
    log_msg(1, "Vulkan: Device destroyed.");
}

static int FM_CALL resize_swapchain_impl(fm_handle h, uint32_t w, uint32_t hgt) {
    Device* d = H2D(h); if (!d) return FM_E_BADARGS;
    if (w == 0 || hgt == 0) { d->needs_recreate.store(true); return FM_OK; }
    d->needs_recreate.store(true);
    return recreate_swapchain(d) ? FM_OK : FM_E_DEVICE;
}

static int FM_CALL begin_frame_impl(fm_handle h, float r, float g, float b, float a) {
    Device* d = H2D(h); if (!d) return FM_E_BADARGS;

    VkExtent2D ce = client_extent(d->hwnd);
    if (ce.width == 0 || ce.height == 0) return FM_E_NOTREADY;

    if (d->needs_recreate.load()) {
        if (!recreate_swapchain(d)) return FM_E_NOTREADY;
    }

    vkWaitForFences(d->device, 1, &d->fence, VK_TRUE, UINT64_MAX);
    vkResetFences(d->device, 1, &d->fence);

    uint32_t idx = 0;
    VkResult aq = vkAcquireNextImageKHR(d->device, d->swap, UINT64_MAX, d->semAcquire, VK_NULL_HANDLE, &idx);
    if (aq == VK_ERROR_OUT_OF_DATE_KHR || aq == VK_SUBOPTIMAL_KHR) {
        d->needs_recreate.store(true);
        return FM_E_OUTOFDATE;
    }
    else if (aq != VK_SUCCESS) {
        g_last_error = "vkAcquireNextImageKHR failed";
        return FM_E_DEVICE;
    }
    d->curImg = idx;

    VkCommandBuffer cb = d->cbs[idx];
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cb, &bi);

    VkClearValue clear{};
    clear.color.float32[0] = r; clear.color.float32[1] = g;
    clear.color.float32[2] = b; clear.color.float32[3] = a;

    VkRenderPassBeginInfo rbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = d->rp;
    rbi.framebuffer = d->fbs[idx];
    rbi.renderArea = { {0,0}, d->extent };
    rbi.clearValueCount = 1; rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    // dynamic viewport/scissor/line width
    VkViewport vp{ 0.f, 0.f, (float)d->extent.width, (float)d->extent.height, 0.f, 1.f };
    VkRect2D   sc{ {0,0}, d->extent };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);

    return FM_OK;
}

static int FM_CALL end_frame_impl(fm_handle h) {
    Device* d = H2D(h); if (!d) return FM_E_BADARGS;
    if (d->cbs.empty()) return FM_OK;

    VkCommandBuffer cb = d->cbs[d->curImg];

    // Record optional line draw if requested this frame
    if (d->pending_draw && d->verts_this_frame > 0) {
        VkDeviceSize off = 0;
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d->pipe);
        vkCmdBindVertexBuffers(cb, 0, 1, &d->vbuf, &off);
        vkCmdPushConstants(cb, d->layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 4, d->color);
        vkCmdDraw(cb, d->verts_this_frame, 1, 0, 0);
    }
    d->pending_draw = false;

    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);

    // submit
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &d->semAcquire; si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &d->semRender;
    vkQueueSubmit(d->gfxQ, 1, &si, d->fence);

    // reset VB usage for next frame
    d->vused = 0; d->verts_this_frame = 0;
    return FM_OK;
}

static int FM_CALL present_impl(fm_handle h) {
    Device* d = H2D(h); if (!d) return FM_E_BADARGS;

    VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    VkSwapchainKHR sw = d->swap; uint32_t idx = d->curImg;
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &d->semRender;
    pi.swapchainCount = 1; pi.pSwapchains = &sw; pi.pImageIndices = &idx;

    VkResult pr = vkQueuePresentKHR(d->gfxQ, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        d->needs_recreate.store(true);
        return FM_E_OUTOFDATE;
    }
    return (pr == VK_SUCCESS) ? FM_OK : FM_E_DEVICE;
}

static int FM_CALL set_matrices_impl(fm_handle h,
    const double* view3x4,
    const double* proj4x4,
    const double* origin3) {
    Device* d = H2D(h); if (!d) return FM_E_BADARGS;
    if (view3x4) std::memcpy(d->view3x4, view3x4, sizeof(double) * 12);
    if (proj4x4) std::memcpy(d->proj4x4, proj4x4, sizeof(double) * 16);
    if (origin3) std::memcpy(d->origin3, origin3, sizeof(double) * 3);
    // NOTE: current simple shaders don't consume these yet (NDC path).
    // In the next step we’ll switch to a camera-relative VS that uses these.
    return FM_OK;
}

static int FM_CALL draw_lines_impl(fm_handle h,
    const float* xyz,
    uint32_t count,
    float r, float g, float b, float a,
    float line_width_pixels) {
    Device* d = H2D(h); if (!d) return FM_E_BADARGS;
    if (!xyz || count == 0) return FM_OK;

    size_t need = (size_t)count * sizeof(float) * 3;
    if (need > d->vcap) { g_last_error = "vertex buffer too small"; return FM_E_NOMEM; }

    std::memcpy(d->mapped, xyz, need);
    d->vused = need;
    d->verts_this_frame = count;
    d->pending_draw = true;
    d->color[0] = r; d->color[1] = g; d->color[2] = b; d->color[3] = a;
    // We enabled dynamic line width; drivers accept 1.0 without wide-lines feature.
    // If you later enable wide-lines, you can call vkCmdSetLineWidth from end_frame.
    (void)line_width_pixels;
    return FM_OK;
}

//========== exported table ==========
extern "C" FM_API void* FM_CALL fmGetRendererAPI(uint32_t requested_abi) {
    if (requested_abi != FM_ABI_VERSION) return nullptr;

    g_api = {};
    g_api.hdr.abi_version = FM_ABI_VERSION;
    g_api.hdr.get_last_error = &get_last_error_cstr;
    g_api.hdr.log = nullptr;
    g_api.hdr.log_user = nullptr;

    g_api.set_logger = &set_logger_impl;
    g_api.create_device = &create_device_impl;
    g_api.destroy_device = &destroy_device_impl;
    g_api.resize_swapchain = &resize_swapchain_impl;

    g_api.begin_frame = &begin_frame_impl;
    g_api.end_frame = &end_frame_impl;
    g_api.present = &present_impl;

    g_api.set_matrices = &set_matrices_impl;
    g_api.draw_lines = &draw_lines_impl;

    return &g_api;
}
