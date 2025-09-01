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

// ===== SPIR-V blobs emitted by your shader step =====
static const uint32_t VS_SPV[] = {
#   include "shaders/vs_ndc_passthrough.spv.inc"
};
static const uint32_t FS_SPV[] = {
#   include "shaders/fs_solid_color.spv.inc"
};
static const uint32_t VS3D_SPV[] = {
#   include "shaders/vs_lines_world.spv.inc"
};

// sanity checks
static_assert(sizeof(VS_SPV) % 4 == 0, "VS_SPV must be dword-aligned");
static_assert(sizeof(FS_SPV) % 4 == 0, "FS_SPV must be dword-aligned");
static constexpr uint32_t VS_SPV_DWORD_COUNT = (uint32_t)(sizeof(VS_SPV) / sizeof(uint32_t));
static constexpr uint32_t FS_SPV_DWORD_COUNT = (uint32_t)(sizeof(FS_SPV) / sizeof(uint32_t));
static constexpr uint32_t VS3D_SPV_DWORD_COUNT = (uint32_t)(sizeof(VS3D_SPV) / sizeof(uint32_t));

// ===== global API + logging =====
static thread_local std::string g_last_error;
static const char* get_last_error() { return g_last_error.c_str(); }

static fw_renderer_api g_api{};
static void log_msg(int level, const char* msg) {
    if (g_api.hdr.log_cb) {
        reinterpret_cast<fw_log_fn>(g_api.hdr.log_cb)(level, msg, g_api.hdr.log_user);
    }
}

// ===== helpers =====
static uint32_t find_memtype(VkPhysicalDevice phys, uint32_t type_bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static bool pick_graphics_queue_family(VkPhysicalDevice pd, uint32_t& family) {
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
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return f;
    }
    return formats[0];
}
static VkPresentModeKHR choose_present_mode(const std::vector<VkPresentModeKHR>& modes) {
    for (auto m : modes) if (m == VK_PRESENT_MODE_MAILBOX_KHR)   return m;
    for (auto m : modes) if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) return m;
    return VK_PRESENT_MODE_FIFO_KHR;
}
static VkExtent2D choose_extent(const VkSurfaceCapabilitiesKHR& caps, HWND hwnd) {
    if (caps.currentExtent.width != UINT32_MAX) return caps.currentExtent;
    RECT rc{}; GetClientRect(hwnd, &rc);
    VkExtent2D e{ (uint32_t)(rc.right - rc.left), (uint32_t)(rc.bottom - rc.top) };
    if (e.width < caps.minImageExtent.width) e.width = caps.minImageExtent.width;
    if (e.height < caps.minImageExtent.height) e.height = caps.minImageExtent.height;
    if (e.width > caps.maxImageExtent.width) e.width = caps.maxImageExtent.width;
    if (e.height > caps.maxImageExtent.height) e.height = caps.maxImageExtent.height;
    return e;
}
static inline VkExtent2D client_extent(HWND hwnd) {
    RECT rc{}; GetClientRect(hwnd, &rc);
    return VkExtent2D{ (uint32_t)(rc.right - rc.left), (uint32_t)(rc.bottom - rc.top) };
}

// ===== device state =====
struct Device {
    HWND            hwnd = nullptr;

    VkInstance       instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice         device = VK_NULL_HANDLE;
    uint32_t         gfxFam = 0xFFFFFFFF;
    VkQueue          gfxQ = VK_NULL_HANDLE;

    VkSurfaceKHR     surface = VK_NULL_HANDLE;
    VkSwapchainKHR   swap = VK_NULL_HANDLE;
    VkFormat         swapFmt = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D       extent{ 0,0 };
    std::vector<VkImage>     images;
    std::vector<VkImageView> views;

    VkRenderPass                 rp = VK_NULL_HANDLE;
    std::vector<VkFramebuffer>   fbs;

    VkCommandPool                cmdPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cbs;
    VkSemaphore semAcquire = VK_NULL_HANDLE;
    VkSemaphore semRender = VK_NULL_HANDLE;
    VkFence     fence = VK_NULL_HANDLE;
    uint32_t    curImg = 0;

    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline       pipe = VK_NULL_HANDLE;

    VkBuffer         vbuf = VK_NULL_HANDLE;
    VkDeviceMemory   vmem = VK_NULL_HANDLE;
    size_t           vcap = 0;
    size_t           vused = 0;
    void* mapped = nullptr;

    float            color[4]{ 1,1,1,1 };

    bool             needs_recreate = false;
};

// fw_handle (uint64) <-> pointer helpers
static inline Device* H2D(fw_handle h) { return reinterpret_cast<Device*>(static_cast<uintptr_t>(h)); }
static inline fw_handle D2H(Device* p) { return static_cast<fw_handle>(reinterpret_cast<uintptr_t>(p)); }

// ===== pipeline + buffer creation =====
static bool create_lines_pipeline(Device* d) {
    // Push constants still only carry color (fragment stage), unchanged.
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
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

    VkVertexInputBindingDescription bind{}; bind.binding = 0; bind.stride = sizeof(float) * 2; bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attr{}; attr.location = 0; attr.binding = 0; attr.format = VK_FORMAT_R32G32_SFLOAT; attr.offset = 0;

    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 1; vi.pVertexAttributeDescriptions = &attr;

    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

    // Dynamic viewport/scissor (Step 1)
    VkPipelineViewportStateCreateInfo vpci{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vpci.viewportCount = 1; vpci.scissorCount = 1;

    VkDynamicState dynStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynStates;

    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;

    VkGraphicsPipelineCreateInfo gp{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.stageCount = 2; gp.pStages = stages;
    gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vpci;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pColorBlendState = &cb;
    gp.pDynamicState = &dyn;               // enable dynamic state
    gp.layout = d->layout; gp.renderPass = d->rp; gp.subpass = 0;

    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult pr = vkCreateGraphicsPipelines(d->device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe);
    vkDestroyShaderModule(d->device, vs, nullptr);
    vkDestroyShaderModule(d->device, fs, nullptr);
    if (pr != VK_SUCCESS) return false;

    d->pipe = pipe;
    return true;
}

static bool create_vertex_buffer(Device* d, size_t min_bytes) {
    if (min_bytes < 65536) min_bytes = 65536;

    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = min_bytes; bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
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

// ===== swapchain (re)creation helpers =====
static void destroy_swapchain_objects(Device* d) {
    for (auto fb : d->fbs) if (fb) vkDestroyFramebuffer(d->device, fb, nullptr);
    d->fbs.clear();
    if (d->rp) { /* keep render pass alive (format likely same) */ }
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
    // Query caps each time (window size may have changed)
    VkSurfaceCapabilitiesKHR caps{}; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d->phys, d->surface, &caps);
    uint32_t fmtCount = 0, pmCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(d->phys, d->surface, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(d->phys, d->surface, &fmtCount, formats.data());
    vkGetPhysicalDeviceSurfacePresentModesKHR(d->phys, d->surface, &pmCount, nullptr);
    std::vector<VkPresentModeKHR> modes(pmCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(d->phys, d->surface, &pmCount, modes.data());

    VkSurfaceFormatKHR sf = choose_surface_format(formats);
    VkPresentModeKHR   pm = choose_present_mode(modes);
    VkExtent2D         ex = choose_extent(caps, d->hwnd);

    // If minimized (0x0), delay creation
    if (ex.width == 0 || ex.height == 0) {
        return false;
    }

    uint32_t imgCount = caps.minImageCount + 1;
    if (caps.maxImageCount && imgCount > caps.maxImageCount) imgCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci2{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sci2.surface = d->surface; sci2.minImageCount = imgCount; sci2.imageFormat = sf.format; sci2.imageColorSpace = sf.colorSpace;
    sci2.imageExtent = ex; sci2.imageArrayLayers = 1; sci2.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci2.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; sci2.preTransform = caps.currentTransform;
    sci2.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci2.presentMode = pm; sci2.clipped = VK_TRUE;

    VkSwapchainKHR swap = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(d->device, &sci2, nullptr, &swap) != VK_SUCCESS) {
        g_last_error = "vkCreateSwapchainKHR(recreate) failed";
        return false;
    }

    d->swap = swap;
    d->swapFmt = sf.format;
    d->extent = ex;

    uint32_t ic = 0; vkGetSwapchainImagesKHR(d->device, swap, &ic, nullptr);
    d->images.resize(ic); vkGetSwapchainImagesKHR(d->device, swap, &ic, d->images.data());
    d->views.resize(ic);

    for (uint32_t i = 0; i < ic; ++i) {
        VkImageViewCreateInfo iv{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        iv.image = d->images[i]; iv.viewType = VK_IMAGE_VIEW_TYPE_2D; iv.format = sf.format;
        iv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; iv.subresourceRange.levelCount = 1; iv.subresourceRange.layerCount = 1;
        if (vkCreateImageView(d->device, &iv, nullptr, &d->views[i]) != VK_SUCCESS) {
            g_last_error = "vkCreateImageView(recreate) failed";
            return false;
        }
    }

    // Framebuffers
    d->fbs.resize(ic);
    for (uint32_t i = 0; i < ic; ++i) {
        VkImageView att[]{ d->views[i] };
        VkFramebufferCreateInfo fbci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fbci.renderPass = d->rp; fbci.attachmentCount = 1; fbci.pAttachments = att; fbci.width = ex.width; fbci.height = ex.height; fbci.layers = 1;
        if (vkCreateFramebuffer(d->device, &fbci, nullptr, &d->fbs[i]) != VK_SUCCESS) {
            g_last_error = "vkCreateFramebuffer(recreate) failed";
            return false;
        }
    }

    // Command buffers per image
    d->cbs.resize(ic);
    VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = d->cmdPool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = ic;
    if (vkAllocateCommandBuffers(d->device, &cbai, d->cbs.data()) != VK_SUCCESS) {
        g_last_error = "vkAllocateCommandBuffers(recreate) failed";
        return false;
    }

    return true;
}

static bool recreate_swapchain(Device* d) {
    // Avoid thrashing while the user is dragging to zero-size
    VkExtent2D ce = client_extent(d->hwnd);
    if (ce.width == 0 || ce.height == 0) return false;

    vkDeviceWaitIdle(d->device);
    destroy_swapchain_objects(d);
    if (!create_swapchain_objects(d)) return false;

    log_msg(1, "Vulkan: Swapchain recreated.");
    d->needs_recreate = false;
    return true;
}

// ===== API functions =====
static void FW_CALL set_logger_impl(void* cb, void* user) {
    g_api.hdr.log_cb = cb; g_api.hdr.log_user = user;
    if (cb) reinterpret_cast<fw_log_fn>(cb)(1, "Logger installed (ABI v3).", user);
}

static int FW_CALL lines_upload_dev(fw_handle hdev, const float* xy, uint32_t count,
    float r, float g, float b, float a) {
    auto* d = H2D(hdev);
    if (!d) { g_last_error = "null device"; return -1; }
    if (!xy || count == 0) return 0;

    size_t need = (size_t)count * sizeof(float) * 2;
    if (need > d->vcap) { g_last_error = "lines buffer overflow"; return -1; }

    std::memcpy(d->mapped, xy, need);
    d->vused = need;
    d->color[0] = r; d->color[1] = g; d->color[2] = b; d->color[3] = a;
    return 0;
}

static int FW_CALL create_device(const fw_renderer_desc* desc, fw_handle* out) {
    *out = 0;
    HWND hwnd = (HWND)desc->hwnd;
    if (!hwnd) { g_last_error = "Null HWND"; return -10; }

    // Instance
    const char* instExts[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "SolarFramework"; app.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = (uint32_t)(sizeof(instExts) / sizeof(instExts[0]));
    ici.ppEnabledExtensionNames = instExts;

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) { g_last_error = "vkCreateInstance failed"; return -1; }
    log_msg(1, "Vulkan: Instance created.");

    // Surface
    VkWin32SurfaceCreateInfoKHR sci{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    sci.hinstance = (HINSTANCE)GetModuleHandleW(nullptr); sci.hwnd = hwnd;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (vkCreateWin32SurfaceKHR(instance, &sci, nullptr, &surface) != VK_SUCCESS) {
        g_last_error = "vkCreateWin32SurfaceKHR failed"; vkDestroyInstance(instance, nullptr); return -2;
    }

    // Physical + queue
    uint32_t n = 0; vkEnumeratePhysicalDevices(instance, &n, nullptr);
    if (!n) { g_last_error = "No GPUs"; vkDestroySurfaceKHR(instance, surface, nullptr); vkDestroyInstance(instance, nullptr); return -3; }
    std::vector<VkPhysicalDevice> devs(n); vkEnumeratePhysicalDevices(instance, &n, devs.data());

    VkPhysicalDevice phys = VK_NULL_HANDLE; uint32_t fam = 0xFFFFFFFF;
    for (auto pd : devs) { uint32_t tmp; if (pick_graphics_queue_family(pd, tmp) && supports_present(pd, tmp, surface)) { phys = pd; fam = tmp; break; } }
    if (!phys) { g_last_error = "No device with graphics+present"; vkDestroySurfaceKHR(instance, surface, nullptr); vkDestroyInstance(instance, nullptr); return -4; }

    // Logical device
    float prio = 1.f;
    VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = fam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    const char* devExts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)(sizeof(devExts) / sizeof(devExts[0]));
    dci.ppEnabledExtensionNames = devExts;
    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(phys, &dci, nullptr, &device) != VK_SUCCESS) {
        g_last_error = "vkCreateDevice failed"; vkDestroySurfaceKHR(instance, surface, nullptr); vkDestroyInstance(instance, nullptr); return -5;
    }
    VkQueue q = VK_NULL_HANDLE; vkGetDeviceQueue(device, fam, 0, &q);

    // Render pass (format chosen during initial swapchain create below)
    // We'll create swapchain first to know the format, then render pass.
    auto* d = new Device();
    d->hwnd = hwnd;
    d->instance = instance; d->phys = phys; d->device = device; d->gfxFam = fam; d->gfxQ = q;
    d->surface = surface;

    // Command pool & sync
    VkCommandPoolCreateInfo cpci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }; cpci.queueFamilyIndex = fam;
    vkCreateCommandPool(device, &cpci, nullptr, &d->cmdPool);
    VkSemaphoreCreateInfo sciS{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateSemaphore(device, &sciS, nullptr, &d->semAcquire);
    vkCreateSemaphore(device, &sciS, nullptr, &d->semRender);
    vkCreateFence(device, &fci, nullptr, &d->fence);

    // Create initial swapchain/images/views/CBs
    if (!create_swapchain_objects(d)) {
        destroy_swapchain_objects(d);
        vkDestroySemaphore(device, d->semRender, nullptr);
        vkDestroySemaphore(device, d->semAcquire, nullptr);
        vkDestroyFence(device, d->fence, nullptr);
        vkDestroyCommandPool(device, d->cmdPool, nullptr);
        vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        delete d;
        return -6;
    }

    // Render pass depends on format; create once now.
    VkAttachmentDescription color{}; color.format = d->swapFmt; color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference cref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &cref;
    VkSubpassDependency dep{}; dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpci.attachmentCount = 1; rpci.pAttachments = &color; rpci.subpassCount = 1; rpci.pSubpasses = &sub;
    rpci.dependencyCount = 1; rpci.pDependencies = &dep;
    if (vkCreateRenderPass(device, &rpci, nullptr, &d->rp) != VK_SUCCESS) {
        g_last_error = "vkCreateRenderPass failed";
        destroy_swapchain_objects(d);
        vkDestroySemaphore(device, d->semRender, nullptr);
        vkDestroySemaphore(device, d->semAcquire, nullptr);
        vkDestroyFence(device, d->fence, nullptr);
        vkDestroyCommandPool(device, d->cmdPool, nullptr);
        vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        delete d;
        return -7;
    }

    // Rebuild framebuffers now that render pass exists (swapchain already made them)
    // Destroy old FBs (if any) and recreate with rp
    for (auto fb : d->fbs) if (fb) vkDestroyFramebuffer(device, fb, nullptr);
    d->fbs.resize(d->views.size());
    for (uint32_t i = 0; i < d->views.size(); ++i) {
        VkImageView att[]{ d->views[i] };
        VkFramebufferCreateInfo fbci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fbci.renderPass = d->rp; fbci.attachmentCount = 1; fbci.pAttachments = att; fbci.width = d->extent.width; fbci.height = d->extent.height; fbci.layers = 1;
        if (vkCreateFramebuffer(device, &fbci, nullptr, &d->fbs[i]) != VK_SUCCESS) {
            g_last_error = "vkCreateFramebuffer (initial) failed";
            // cleanup falls through below
        }
    }

    if (!create_vertex_buffer(d, size_t{ 1 } << 20)) {
        g_last_error = "lines pipeline/buffer failed";
    }

    *out = D2H(d);
    log_msg(1, "Vulkan: swapchain + lines pipeline ready.");
    return 0;
}

static void FW_CALL destroy_device(fw_handle h) {
    auto* d = H2D(h); if (!d) return;
    vkDeviceWaitIdle(d->device);

    if (d->vmem)   vkUnmapMemory(d->device, d->vmem);
    if (d->vbuf)   vkDestroyBuffer(d->device, d->vbuf, nullptr);
    if (d->vmem)   vkFreeMemory(d->device, d->vmem, nullptr);
    if (d->pipe)   vkDestroyPipeline(d->device, d->pipe, nullptr);
    if (d->layout) vkDestroyPipelineLayout(d->device, d->layout, nullptr);

    if (d->fence)      vkDestroyFence(d->device, d->fence, nullptr);
    if (d->semRender)  vkDestroySemaphore(d->device, d->semRender, nullptr);
    if (d->semAcquire) vkDestroySemaphore(d->device, d->semAcquire, nullptr);

    destroy_swapchain_objects(d);
    if (d->rp) vkDestroyRenderPass(d->device, d->rp, nullptr);

    if (d->cmdPool) vkDestroyCommandPool(d->device, d->cmdPool, nullptr);
    if (d->surface) vkDestroySurfaceKHR(d->instance, d->surface, nullptr);
    if (d->device)  vkDestroyDevice(d->device, nullptr);
    if (d->instance)vkDestroyInstance(d->instance, nullptr);

    delete d;
    log_msg(1, "Vulkan: Device destroyed.");
}

static void FW_CALL begin_frame(fw_handle h) {
    auto* d = H2D(h); if (!d) return;

    // If window is minimized/zero area, skip drawing this frame to avoid stalls
    VkExtent2D ce = client_extent(d->hwnd);
    if (ce.width == 0 || ce.height == 0) {
        d->vused = 0;
        return;
    }

    // Detect size change; recreate swapchain if needed
    if ((ce.width != d->extent.width || ce.height != d->extent.height))
        d->needs_recreate = true;

    if (d->needs_recreate) {
        if (!recreate_swapchain(d)) {
            // couldn't recreate yet (likely zero size); skip frame
            d->vused = 0;
            return;
        }
        // Framebuffers were rebuilt; pipeline uses dynamic viewport so it's fine
    }

    vkWaitForFences(d->device, 1, &d->fence, VK_TRUE, UINT64_MAX);
    vkResetFences(d->device, 1, &d->fence);

    uint32_t idx = 0;
    VkResult aq = vkAcquireNextImageKHR(d->device, d->swap, UINT64_MAX, d->semAcquire, VK_NULL_HANDLE, &idx);
    if (aq == VK_ERROR_OUT_OF_DATE_KHR || aq == VK_SUBOPTIMAL_KHR) {
        d->needs_recreate = true;
        d->vused = 0;
        return; // will recreate next frame
    }
    else if (aq != VK_SUCCESS) {
        // Some other error; bail quietly this frame
        d->vused = 0;
        return;
    }
    d->curImg = idx;

    VkCommandBuffer cb = d->cbs[idx];
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cb, &bi);

    VkClearValue clear{};                                  // zero-init the union
    clear.color = { { 0.02f, 0.03f, 0.05f, 1.0f } };

    VkRenderPassBeginInfo rbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = d->rp;
    rbi.framebuffer = d->fbs[idx];
    rbi.renderArea = { {0,0}, d->extent };
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;

    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    // Dynamic viewport & scissor each frame
    VkViewport vp{ 0.f, 0.f, (float)d->extent.width, (float)d->extent.height, 0.f, 1.f };
    VkRect2D   sc{ {0,0}, d->extent };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);

    if (d->vused >= sizeof(float) * 2) {
        VkDeviceSize off = 0;
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d->pipe);
        vkCmdBindVertexBuffers(cb, 0, 1, &d->vbuf, &off);
        vkCmdPushConstants(cb, d->layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 4, d->color);
        uint32_t vtx = (uint32_t)(d->vused / (sizeof(float) * 2));
        vkCmdDraw(cb, vtx, 1, 0, 0);
    }

    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);
}

static void FW_CALL end_frame(fw_handle h) {
    auto* d = H2D(h); if (!d) return;

    // If we didn't record (e.g., zero-size/recreate), skip submit/present
    if (d->cbs.empty()) return;

    VkCommandBuffer cb = d->cbs[d->curImg];
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &d->semAcquire; si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &d->semRender;
    vkQueueSubmit(d->gfxQ, 1, &si, d->fence);

    VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    VkSwapchainKHR sw = d->swap; uint32_t idx = d->curImg;
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &d->semRender;
    pi.swapchainCount = 1; pi.pSwapchains = &sw; pi.pImageIndices = &idx;
    VkResult pr = vkQueuePresentKHR(d->gfxQ, &pi);

    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        d->needs_recreate = true;
    }

    d->vused = 0;
}

extern "C" FW_API void* FW_CALL fwGetRendererAPI(uint32_t abi) {
    if (abi != 3) return nullptr;
    g_api = {};
    g_api.hdr.abi_version = 3;
    g_api.hdr.get_last_error = &get_last_error;
    g_api.hdr.log_cb = nullptr;
    g_api.hdr.log_user = nullptr;

    g_api.set_logger = &set_logger_impl;
    g_api.lines_upload = &lines_upload_dev;
    g_api.create_device = &create_device;
    g_api.destroy_device = &destroy_device;
    g_api.begin_frame = &begin_frame;
    g_api.end_frame = &end_frame;
    return &g_api;
}
