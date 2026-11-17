/*
 * oma-zc-client: zero-copy P0, the guest side (Linux, glibc, runs under OmaLauncher's proot).
 *
 * Connects to the launcher's socket (zc_proto.h), receives the dma-buf fds of its
 * AHardwareBuffers, imports them into Vulkan and renders a test pattern into them,
 * then hands each finished buffer back. Nothing is copied by the CPU.
 *
 *   --path image   render straight into the imported image (linear, explicit row pitch)
 *   --path buffer  render into a GPU-local image, then copy it with the GPU into the
 *                  imported memory bound as a VkBuffer (works whenever buffer import does)
 *   --path auto    image, and buffer if image import fails (default)
 *
 * Vulkan comes from the guest's loader (libvulkan.so.1, with OmaLauncher's VK_DRIVER_FILES),
 * or straight from a driver with --icd /path/libvulkan_xxx.so.
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "zc_proto.h"
#include "zc_frag.h"
#include "zc_vert.h"

#define LOG(...) (printf(__VA_ARGS__), putchar('\n'))

/* ---- Vulkan entry points ---- */

#define VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceVersion)
#define VK_INSTANCE(X)                                                                                      \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)                    \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceQueueFamilyProperties)                           \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkEnumerateDeviceExtensionProperties)                          \
    X(vkGetPhysicalDeviceFormatProperties2) X(vkGetPhysicalDeviceImageFormatProperties2)                    \
    X(vkGetPhysicalDeviceExternalBufferProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define VK_DEVICE(X)                                                                                        \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateImage) X(vkDestroyImage)                              \
    X(vkGetImageMemoryRequirements) X(vkGetImageSubresourceLayout) X(vkAllocateMemory) X(vkFreeMemory)      \
    X(vkBindImageMemory) X(vkBindBufferMemory) X(vkCreateBuffer) X(vkDestroyBuffer)                         \
    X(vkGetBufferMemoryRequirements) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateRenderPass)       \
    X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateShaderModule)           \
    X(vkDestroyShaderModule) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout)                           \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateCommandPool) X(vkDestroyCommandPool)        \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer)       \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) X(vkCmdSetViewport)                  \
    X(vkCmdSetScissor) X(vkCmdPushConstants) X(vkCmdDraw) X(vkCmdPipelineBarrier)                           \
    X(vkCmdCopyImageToBuffer) X(vkCreateFence) X(vkDestroyFence) X(vkResetFences) X(vkWaitForFences)        \
    X(vkQueueSubmit) X(vkDeviceWaitIdle)
#define VK_DEVICE_OPTIONAL(X) X(vkGetMemoryFdPropertiesKHR) X(vkGetImageDrmFormatModifierPropertiesEXT)

#define DECLARE(name) static PFN_##name name;
VK_GLOBAL(DECLARE)
VK_INSTANCE(DECLARE)
VK_DEVICE(DECLARE)
VK_DEVICE_OPTIONAL(DECLARE)
static PFN_vkGetInstanceProcAddr gipa;

/* ---- state ---- */

struct target {
    VkImage image;   /* path image: the imported image; path buffer: unused */
    VkBuffer buffer; /* path buffer: the imported memory */
    VkDeviceMemory memory;
    VkImageView view;
    VkFramebuffer fb;
};

static struct {
    const char *socket_path;
    const char *icd;
    int frames;
    enum zc_path want_path;
    int device_index;
    int foreign;
    int has_modifier;
    uint64_t modifier; /* --modifier: import with this instead of the launcher's (e.g. a compressed layout) */
} opt = {.frames = 600, .want_path = ZC_PATH_NONE, .device_index = -1, .foreign = 1};

static int sock = -1;
static struct zc_hello hello;
static struct zc_buffer bufs[ZC_MAX_BUFS];
static int buf_fd[ZC_MAX_BUFS];

static VkInstance instance;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfamily;
static VkPhysicalDeviceMemoryProperties memprops;
static int has_modifier, has_foreign;
static enum zc_path path;
static struct target targets[ZC_MAX_BUFS];
static struct target offscreen; /* path buffer */
static VkRenderPass render_pass;
static VkPipelineLayout layout;
static VkPipeline pipeline;
static VkCommandPool pool;
static VkCommandBuffer cmd;
static VkFence fence;
static char device_name[64] = "?";

static const VkFormat FORMAT = VK_FORMAT_R8G8B8A8_UNORM;

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

#define CHECK(call)                                                       \
    do {                                                                  \
        VkResult r_ = (call);                                             \
        if (r_ != VK_SUCCESS) {                                           \
            LOG("FAIL %s: VkResult %d (line %d)", #call, r_, __LINE__);   \
            return -1;                                                    \
        }                                                                 \
    } while (0)

/* ---- socket ---- */

static ssize_t recv_packet(void *buf, size_t size, int *fd) {
    char cbuf[CMSG_SPACE(sizeof(int) * 4)];
    struct iovec iov = {.iov_base = buf, .iov_len = size};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof(cbuf)};
    ssize_t n;
    do n = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
    while (n < 0 && errno == EINTR);
    if (fd) *fd = -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        int count = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        for (int i = 0; i < count; i++) {
            int f;
            memcpy(&f, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
            if (fd && *fd < 0) *fd = f;
            else close(f);
        }
    }
    return n;
}

static int send_packet(const void *data, size_t size) {
    ssize_t n;
    do n = send(sock, data, size, MSG_NOSIGNAL);
    while (n < 0 && errno == EINTR);
    return n == (ssize_t)size ? 0 : -1;
}

static int connect_launcher(void) {
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", opt.socket_path);
    sock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (sock < 0 || connect(sock, (struct sockaddr *)&addr, sizeof(addr))) {
        LOG("FAIL connect %s: %s", opt.socket_path, strerror(errno));
        return -1;
    }
    if (recv_packet(&hello, sizeof(hello), NULL) != (ssize_t)sizeof(hello) || hello.h.type != ZC_MSG_HELLO) {
        LOG("FAIL no HELLO");
        return -1;
    }
    if (hello.version != ZC_PROTO_VERSION || hello.nbuf < 1 || hello.nbuf > ZC_MAX_BUFS) {
        LOG("FAIL HELLO version %u nbuf %u", hello.version, hello.nbuf);
        return -1;
    }
    LOG("launcher: %ux%u, %u buffers, busy mask %#x", hello.width, hello.height, hello.nbuf, hello.busy_mask);
    for (uint32_t i = 0; i < hello.nbuf; i++) {
        struct zc_buffer b;
        int fd;
        if (recv_packet(&b, sizeof(b), &fd) != (ssize_t)sizeof(b) || b.h.type != ZC_MSG_BUFFER || b.index >= hello.nbuf || fd < 0) {
            LOG("FAIL bad BUFFER %u (fd %d)", i, fd);
            return -1;
        }
        bufs[b.index] = b;
        buf_fd[b.index] = fd;
        off_t size = lseek(fd, 0, SEEK_END);
        char kind[128] = "", link[64];
        snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        ssize_t len = readlink(link, kind, sizeof(kind) - 1);
        kind[len > 0 ? len : 0] = 0;
        LOG("buffer %u: %ux%u stride %u px (%u B) offset %u, dma-buf %llu B (here: %lld, %s), modifier %#llx, "
            "usage %#llx, fd %u of %u",
            b.index, b.width, b.height, b.stride_px, b.stride_bytes, b.offset, (unsigned long long)b.dmabuf_size,
            (long long)size, kind, (unsigned long long)b.modifier, (unsigned long long)b.usage, b.fd_index, b.handle_fds);
    }
    return 0;
}

/* ---- Vulkan setup ---- */

static int load_vulkan(void) {
    void *lib;
    if (opt.icd) {
        lib = dlopen(opt.icd, RTLD_NOW | RTLD_LOCAL);
        if (!lib) {
            LOG("FAIL dlopen %s: %s", opt.icd, dlerror());
            return -1;
        }
        /* vk_icd.h's PFN_vkNegotiateLoaderICDInterfaceVersion; not in older vulkan.h. */
        typedef VkResult (*negotiate_fn)(uint32_t *);
        negotiate_fn negotiate = (negotiate_fn)dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
        uint32_t version = 5;
        if (negotiate) negotiate(&version);
        gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
        LOG("vulkan: driver %s directly (ICD interface %u)", opt.icd, version);
    } else {
        lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!lib) {
            LOG("FAIL dlopen libvulkan.so.1: %s", dlerror());
            return -1;
        }
        gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
        const char *files = getenv("VK_DRIVER_FILES");
        LOG("vulkan: loader, VK_DRIVER_FILES=%s", files ? files : "(unset)");
    }
    if (!gipa) {
        LOG("FAIL no GetInstanceProcAddr");
        return -1;
    }
#define LOAD_GLOBAL(name) name = (PFN_##name)gipa(NULL, #name);
    VK_GLOBAL(LOAD_GLOBAL)
    return vkCreateInstance ? 0 : -1;
}

static int has_ext(const VkExtensionProperties *exts, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(exts[i].extensionName, name)) return 1;
    return 0;
}

static int create_device(void) {
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "oma-zc-client",
                             .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    CHECK(vkCreateInstance(&ici, NULL, &instance));
#define LOAD_INSTANCE(name)                                  \
    name = (PFN_##name)gipa(instance, #name);                \
    if (!name) {                                             \
        LOG("FAIL missing " #name);                          \
        return -1;                                           \
    }
    VK_INSTANCE(LOAD_INSTANCE)

    uint32_t n = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &n, NULL));
    VkPhysicalDevice devs[8];
    if (n > 8) n = 8;
    CHECK(vkEnumeratePhysicalDevices(instance, &n, devs));
    if (!n) {
        LOG("FAIL no Vulkan devices");
        return -1;
    }
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
        vkGetPhysicalDeviceProperties2(devs[i], &p2);
        LOG("device %u: %s, type %d, API %u.%u.%u, driver %s (%s)", i, p2.properties.deviceName, p2.properties.deviceType,
            VK_API_VERSION_MAJOR(p2.properties.apiVersion), VK_API_VERSION_MINOR(p2.properties.apiVersion),
            VK_API_VERSION_PATCH(p2.properties.apiVersion), drv.driverName, drv.driverInfo);
        if (opt.device_index < 0 && p2.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) opt.device_index = (int)i;
        if (opt.device_index == (int)i) snprintf(device_name, sizeof(device_name), "%s", p2.properties.deviceName);
    }
    if (opt.device_index < 0) opt.device_index = 0, snprintf(device_name, sizeof(device_name), "cpu");
    phys = devs[opt.device_index < (int)n ? opt.device_index : 0];

    uint32_t next = 0;
    vkEnumerateDeviceExtensionProperties(phys, NULL, &next, NULL);
    VkExtensionProperties *exts = calloc(next, sizeof(*exts));
    vkEnumerateDeviceExtensionProperties(phys, NULL, &next, exts);
    const char *want[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
    };
    const char *enabled[8];
    uint32_t nenabled = 0;
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        int has = has_ext(exts, next, want[i]);
        LOG("  %-40s %s", want[i], has ? "yes" : "NO");
        if (has) enabled[nenabled++] = want[i];
    }
    LOG("  %-40s %s", "VK_ANDROID_external_memory_android_hardware_buffer",
        has_ext(exts, next, "VK_ANDROID_external_memory_android_hardware_buffer") ? "yes" : "no");
    int fd_ok = has_ext(exts, next, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    int dmabuf_ok = has_ext(exts, next, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    has_modifier = has_ext(exts, next, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    has_foreign = opt.foreign && has_ext(exts, next, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
    free(exts);
    if (!fd_ok || !dmabuf_ok) {
        LOG("FAIL the driver cannot import dma-bufs");
        return -1;
    }

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
    VkQueueFamilyProperties qp[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qp);
    qfamily = UINT32_MAX;
    for (uint32_t i = 0; i < nq && qfamily == UINT32_MAX; i++)
        if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) qfamily = i;
    if (qfamily == UINT32_MAX) {
        LOG("FAIL no graphics queue");
        return -1;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfamily,
                                   .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci, .enabledExtensionCount = nenabled,
                              .ppEnabledExtensionNames = enabled};
    CHECK(vkCreateDevice(phys, &dci, NULL, &dev));
#define LOAD_DEVICE(name)                                    \
    name = (PFN_##name)vkGetDeviceProcAddr(dev, #name);      \
    if (!name) {                                             \
        LOG("FAIL missing " #name);                          \
        return -1;                                           \
    }
    VK_DEVICE(LOAD_DEVICE)
#define LOAD_OPTIONAL(name) name = (PFN_##name)vkGetDeviceProcAddr(dev, #name);
    VK_DEVICE_OPTIONAL(LOAD_OPTIONAL)
    vkGetDeviceQueue(dev, qfamily, 0, &queue);
    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        LOG("memory type %u: flags %#x heap %u", i, memprops.memoryTypes[i].propertyFlags, memprops.memoryTypes[i].heapIndex);
    return 0;
}

/* What the driver says about RGBA8 with DRM modifiers and dma-buf import, for the log. */
static void probe_formats(void) {
    if (has_modifier) {
        VkDrmFormatModifierPropertiesListEXT list = {.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
        VkFormatProperties2 fp = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &list};
        vkGetPhysicalDeviceFormatProperties2(phys, FORMAT, &fp);
        VkDrmFormatModifierPropertiesEXT mods[32];
        if (list.drmFormatModifierCount > 32) list.drmFormatModifierCount = 32;
        list.pDrmFormatModifierProperties = mods;
        vkGetPhysicalDeviceFormatProperties2(phys, FORMAT, &fp);
        for (uint32_t i = 0; i < list.drmFormatModifierCount; i++)
            LOG("RGBA8 modifier %#018llx: planes %u, features %#x", (unsigned long long)mods[i].drmFormatModifier,
                mods[i].drmFormatModifierPlaneCount, mods[i].drmFormatModifierTilingFeatures);
    }
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modinfo = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .drmFormatModifier = ZC_DRM_FORMAT_MOD_LINEAR,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkPhysicalDeviceExternalImageFormatInfo extinfo = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .pNext = has_modifier ? &modinfo : NULL,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkPhysicalDeviceImageFormatInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &extinfo,
        .format = FORMAT,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = has_modifier ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
    };
    VkExternalImageFormatProperties extprops = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 props = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &extprops};
    VkResult r = vkGetPhysicalDeviceImageFormatProperties2(phys, &info, &props);
    LOG("RGBA8 %s color attachment from a dma-buf: VkResult %d, max %ux%u, external features %#x (1 dedicated only, 4 importable)",
        has_modifier ? "linear-modifier" : "linear-tiling", r, props.imageFormatProperties.maxExtent.width,
        props.imageFormatProperties.maxExtent.height, extprops.externalMemoryProperties.externalMemoryFeatures);
    VkPhysicalDeviceExternalBufferInfo binfo = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
                                                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkExternalBufferProperties bprops = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    vkGetPhysicalDeviceExternalBufferProperties(phys, &binfo, &bprops);
    LOG("buffer from a dma-buf: external features %#x", bprops.externalMemoryProperties.externalMemoryFeatures);
}

/* ---- import ---- */

/* A memory type the requirements and the dma-buf both allow; -1 if none. */
static int pick_memory(uint32_t req_bits, int fd) {
    uint32_t fd_bits = UINT32_MAX;
    if (vkGetMemoryFdPropertiesKHR) {
        VkMemoryFdPropertiesKHR fp = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        VkResult r = vkGetMemoryFdPropertiesKHR(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp);
        if (r == VK_SUCCESS) fd_bits = fp.memoryTypeBits;
        else LOG("  vkGetMemoryFdPropertiesKHR: VkResult %d, trying the requirements' types", r);
    }
    uint32_t bits = req_bits & fd_bits;
    LOG("  memory types: required %#x, dma-buf %#x", req_bits, fd_bits);
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        if (bits & (1u << i)) return (int)i;
    return -1;
}

static int import_memory(const struct zc_buffer *b, int fd, VkMemoryRequirements req, VkImage image, VkBuffer buffer,
                         VkDeviceMemory *out) {
    int type = pick_memory(req.memoryTypeBits, fd);
    if (type < 0) {
        LOG("FAIL no memory type");
        return -1;
    }
    VkDeviceSize size = req.size;
    if (size > b->dmabuf_size) LOG("  WARNING: the driver wants %llu bytes, the dma-buf has %llu", (unsigned long long)size,
                                   (unsigned long long)b->dmabuf_size);
    VkMemoryDedicatedAllocateInfo dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                                               .image = image, .buffer = buffer};
    VkImportMemoryFdInfoKHR import = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .pNext = &dedicated,
                                      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = dup(fd)};
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &import,
                                .allocationSize = size, .memoryTypeIndex = (uint32_t)type};
    int64_t t0 = now_ns();
    VkResult r = vkAllocateMemory(dev, &mai, NULL, out);
    if (r != VK_SUCCESS) {
        close(import.fd); /* ownership only moves on success */
        LOG("FAIL import (vkAllocateMemory %llu bytes, type %d): VkResult %d", (unsigned long long)size, type, r);
        return -1;
    }
    LOG("  imported %llu bytes as memory type %d in %.2f ms", (unsigned long long)size, type, (now_ns() - t0) / 1e6);
    return 0;
}

static int import_image(uint32_t i) {
    const struct zc_buffer *b = &bufs[i];
    uint64_t modifier = opt.has_modifier ? opt.modifier : b->modifier;
    struct target *t = &targets[i];
    LOG("buffer %u: importing as an image, modifier %#llx", i, (unsigned long long)(opt.has_modifier ? opt.modifier : b->modifier));
    VkExternalMemoryImageCreateInfo ext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkSubresourceLayout plane = {.offset = b->offset, .size = 0, .rowPitch = b->stride_bytes};
    VkImageDrmFormatModifierExplicitCreateInfoEXT mod = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .pNext = &ext,
        .drmFormatModifier = modifier,
        .drmFormatModifierPlaneCount = 1,
        .pPlaneLayouts = &plane,
    };
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = has_modifier ? (void *)&mod : (void *)&ext,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = FORMAT,
        .extent = {b->width, b->height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = has_modifier ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    CHECK(vkCreateImage(dev, &ici, NULL, &t->image));
    VkImageSubresource sub = {.aspectMask = has_modifier ? VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT : VK_IMAGE_ASPECT_COLOR_BIT};
    VkSubresourceLayout got;
    vkGetImageSubresourceLayout(dev, t->image, &sub, &got);
    LOG("  driver layout: offset %llu row pitch %llu size %llu", (unsigned long long)got.offset,
        (unsigned long long)got.rowPitch, (unsigned long long)got.size);
    if (modifier == ZC_DRM_FORMAT_MOD_LINEAR && (got.rowPitch != b->stride_bytes || got.offset != b->offset)) {
        LOG("FAIL the driver's layout differs from gralloc's (row pitch %u, offset %u)", b->stride_bytes, b->offset);
        return -1;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, t->image, &req);
    if (import_memory(b, buf_fd[i], req, t->image, VK_NULL_HANDLE, &t->memory)) return -1;
    CHECK(vkBindImageMemory(dev, t->image, t->memory, 0));
    return 0;
}

static int import_buffer(uint32_t i) {
    const struct zc_buffer *b = &bufs[i];
    struct target *t = &targets[i];
    LOG("buffer %u: importing as a buffer", i);
    VkExternalMemoryBufferCreateInfo ext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &ext,
                              .size = (VkDeviceSize)b->offset + (VkDeviceSize)b->stride_bytes * b->height,
                              .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    CHECK(vkCreateBuffer(dev, &bci, NULL, &t->buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, t->buffer, &req);
    if (import_memory(b, buf_fd[i], req, VK_NULL_HANDLE, t->buffer, &t->memory)) return -1;
    CHECK(vkBindBufferMemory(dev, t->buffer, t->memory, 0));
    return 0;
}

static void drop_target(struct target *t) {
    if (t->fb) vkDestroyFramebuffer(dev, t->fb, NULL);
    if (t->view) vkDestroyImageView(dev, t->view, NULL);
    if (t->image) vkDestroyImage(dev, t->image, NULL);
    if (t->buffer) vkDestroyBuffer(dev, t->buffer, NULL);
    if (t->memory) vkFreeMemory(dev, t->memory, NULL);
    memset(t, 0, sizeof(*t));
}

/* The GPU-local image the buffer path renders into before its copy. */
static int create_offscreen(void) {
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = FORMAT,
        .extent = {hello.width, hello.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    CHECK(vkCreateImage(dev, &ici, NULL, &offscreen.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, offscreen.image, &req);
    int type = -1;
    for (uint32_t i = 0; i < memprops.memoryTypeCount && type < 0; i++)
        if ((req.memoryTypeBits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            type = (int)i;
    for (uint32_t i = 0; i < memprops.memoryTypeCount && type < 0; i++)
        if (req.memoryTypeBits & (1u << i)) type = (int)i;
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                .memoryTypeIndex = (uint32_t)type};
    CHECK(vkAllocateMemory(dev, &mai, NULL, &offscreen.memory));
    CHECK(vkBindImageMemory(dev, offscreen.image, offscreen.memory, 0));
    return 0;
}

static int import_all(void) {
    enum zc_path order[2] = {ZC_PATH_IMAGE, ZC_PATH_BUFFER};
    int tries = 2;
    if (opt.want_path != ZC_PATH_NONE) order[0] = opt.want_path, tries = 1;
    for (int k = 0; k < tries; k++) {
        path = order[k];
        uint32_t i;
        for (i = 0; i < hello.nbuf; i++)
            if ((path == ZC_PATH_IMAGE ? import_image(i) : import_buffer(i))) break;
        if (i == hello.nbuf && (path == ZC_PATH_IMAGE || !create_offscreen())) {
            LOG("path: %s", path == ZC_PATH_IMAGE ? "image (render into the imported buffer)" : "buffer (render, then GPU copy into the imported buffer)");
            return 0;
        }
        for (uint32_t j = 0; j < hello.nbuf; j++) drop_target(&targets[j]);
        if (k + 1 < tries) LOG("path %s failed, trying the next", path == ZC_PATH_IMAGE ? "image" : "buffer");
    }
    path = ZC_PATH_NONE;
    return -1;
}

/* ---- rendering ---- */

static int create_pipeline(void) {
    VkAttachmentDescription att = {
        .format = FORMAT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = path == ZC_PATH_IMAGE ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    };
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
                                .pColorAttachments = &ref};
    VkRenderPassCreateInfo rpci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
                                   .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub};
    CHECK(vkCreateRenderPass(dev, &rpci, NULL, &render_pass));

    struct target *list = path == ZC_PATH_IMAGE ? targets : &offscreen;
    uint32_t count = path == ZC_PATH_IMAGE ? hello.nbuf : 1;
    for (uint32_t i = 0; i < count; i++) {
        VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = list[i].image,
                                     .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = FORMAT,
                                     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        CHECK(vkCreateImageView(dev, &vci, NULL, &list[i].view));
        VkFramebufferCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = render_pass,
                                       .attachmentCount = 1, .pAttachments = &list[i].view, .width = hello.width,
                                       .height = hello.height, .layers = 1};
        CHECK(vkCreateFramebuffer(dev, &fci, NULL, &list[i].fb));
    }

    VkShaderModule vs, fs;
    VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(zc_vert_spv),
                                     .pCode = zc_vert_spv};
    CHECK(vkCreateShaderModule(dev, &smci, NULL, &vs));
    smci.codeSize = sizeof(zc_frag_spv);
    smci.pCode = zc_frag_spv;
    CHECK(vkCreateShaderModule(dev, &smci, NULL, &fs));
    VkPushConstantRange pcr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
    CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"},
    };
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                 .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1, .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb,
        .pDynamicState = &ds, .layout = layout, .renderPass = render_pass,
    };
    CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipeline));
    vkDestroyShaderModule(dev, vs, NULL);
    vkDestroyShaderModule(dev, fs, NULL);

    VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qfamily};
    CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
    VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(dev, &fci, NULL, &fence));
    return 0;
}

static int render(uint32_t idx, uint32_t frame) {
    const struct zc_buffer *b = &bufs[idx];
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkResetCommandBuffer(cmd, 0));
    CHECK(vkBeginCommandBuffer(cmd, &bi));
    VkRenderPassBeginInfo rpbi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = render_pass,
        .framebuffer = path == ZC_PATH_IMAGE ? targets[idx].fb : offscreen.fb,
        .renderArea = {{0, 0}, {hello.width, hello.height}},
    };
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkViewport viewport = {0, 0, (float)hello.width, (float)hello.height, 0, 1};
    VkRect2D scissor = {{0, 0}, {hello.width, hello.height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    uint32_t pc[4] = {frame, hello.width, hello.height, 0};
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    uint32_t dst_family = has_foreign ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_IGNORED;
    uint32_t src_family = has_foreign ? qfamily : VK_QUEUE_FAMILY_IGNORED;
    if (path == ZC_PATH_IMAGE) {
        /* Done writing: hand the image to the compositor (another device, as far as Vulkan knows). */
        VkImageMemoryBarrier ib = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = src_family,
            .dstQueueFamilyIndex = dst_family,
            .image = targets[idx].image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                             0, NULL, 0, NULL, 1, &ib);
    } else {
        VkBufferImageCopy region = {
            .bufferOffset = b->offset,
            .bufferRowLength = b->stride_px,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {hello.width, hello.height, 1},
        };
        vkCmdCopyImageToBuffer(cmd, offscreen.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, targets[idx].buffer, 1, &region);
        VkBufferMemoryBarrier bb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .srcQueueFamilyIndex = src_family,
            .dstQueueFamilyIndex = dst_family,
            .buffer = targets[idx].buffer,
            .size = VK_WHOLE_SIZE,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 1,
                             &bb, 0, NULL);
    }
    CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
    CHECK(vkQueueSubmit(queue, 1, &si, fence));
    /* Stage 1: wait for the GPU here instead of passing a fence to the compositor. */
    CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull));
    CHECK(vkResetFences(dev, 1, &fence));
    return 0;
}

/* Reads launcher messages; blocks until at least one buffer is free when [block]. -1 when the launcher left. */
static int pump(uint32_t *free_mask, int block) {
    for (;;) {
        struct pollfd pfd = {.fd = sock, .events = POLLIN};
        int r = poll(&pfd, 1, block && !*free_mask ? 3000 : 0);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return -1;
        if (r == 0) {
            if (block && !*free_mask) {
                LOG("waiting for the launcher to release a buffer");
                continue;
            }
            return 0;
        }
        union {
            struct zc_hdr h;
            struct zc_release release;
            char raw[256];
        } m;
        ssize_t n = recv_packet(&m, sizeof(m), NULL);
        if (n <= 0) return -1;
        if (m.h.type == ZC_MSG_RELEASE && m.release.index < hello.nbuf) *free_mask |= 1u << m.release.index;
        else if (m.h.type == ZC_MSG_BYE) return -1;
    }
}

static int run(void) {
    uint32_t all = (1u << hello.nbuf) - 1, free_mask = all & ~hello.busy_mask;
    int64_t t_start = now_ns(), t_report = t_start, render_sum = 0;
    uint32_t window = 0, next = 0;
    for (uint32_t frame = 0; opt.frames <= 0 || frame < (uint32_t)opt.frames; frame++) {
        if (pump(&free_mask, 1)) {
            LOG("the launcher went away");
            return -1;
        }
        /* Round robin among the free ones, so every buffer gets used. */
        uint32_t idx = next;
        while (!(free_mask & (1u << idx))) idx = (idx + 1) % hello.nbuf;
        next = (idx + 1) % hello.nbuf;
        free_mask &= ~(1u << idx);
        int64_t t0 = now_ns();
        if (render(idx, frame)) return -1;
        int64_t t1 = now_ns();
        struct zc_present p = {{ZC_MSG_PRESENT, sizeof(p)}, idx, frame, (uint64_t)(t1 - t0)};
        if (send_packet(&p, sizeof(p))) {
            LOG("the launcher went away");
            return -1;
        }
        render_sum += t1 - t0;
        window++;
        if (t1 - t_report > 2000000000LL) {
            LOG("frame %u: %.1f fps, render+wait %.2f ms avg", frame, window / ((t1 - t_report) / 1e9),
                render_sum / 1e6 / window);
            t_report = t1;
            render_sum = 0;
            window = 0;
        }
    }
    LOG("done: %d frames in %.1f s", opt.frames, (now_ns() - t_start) / 1e9);
    return 0;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s --socket PATH [--frames N (0: until the launcher goes)] [--path auto|image|buffer]\n"
            "          [--icd /path/libvulkan_driver.so] [--device N] [--no-foreign] [--modifier 0x...]\n",
            argv0);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--socket") && v) opt.socket_path = v, i++;
        else if (!strcmp(a, "--icd") && v) opt.icd = v, i++;
        else if (!strcmp(a, "--frames") && v) opt.frames = atoi(v), i++;
        else if (!strcmp(a, "--device") && v) opt.device_index = atoi(v), i++;
        else if (!strcmp(a, "--no-foreign")) opt.foreign = 0;
        else if (!strcmp(a, "--modifier") && v) opt.has_modifier = 1, opt.modifier = strtoull(v, NULL, 0), i++;
        else if (!strcmp(a, "--path") && v) {
            opt.want_path = !strcmp(v, "image") ? ZC_PATH_IMAGE : !strcmp(v, "buffer") ? ZC_PATH_BUFFER : ZC_PATH_NONE;
            i++;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!opt.socket_path) {
        usage(argv[0]);
        return 2;
    }
    if (connect_launcher() || load_vulkan() || create_device()) return 1;
    probe_formats();
    int imported = import_all();
    struct zc_ready ready = {{ZC_MSG_READY, sizeof(ready)}, imported == 0, (uint32_t)path, ""};
    snprintf(ready.device, sizeof(ready.device), "%s", device_name);
    send_packet(&ready, sizeof(ready));
    if (imported) {
        LOG("FAIL no import path worked");
        return 1;
    }
    if (create_pipeline()) return 1;
    int r = run();
    struct zc_bye bye = {{ZC_MSG_BYE, sizeof(bye)}, (uint32_t)(r ? 1 : 0)};
    send_packet(&bye, sizeof(bye));
    vkDeviceWaitIdle(dev);
    return r ? 1 : 0;
}
