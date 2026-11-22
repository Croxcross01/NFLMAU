void DumpGuestThreads();
// Game window + Vulkan presenter. Each XE_SWAP hands us the front buffer (a guest texture); we untile it,
// convert it to RGBA8, upload it and scale it into the window (aspect preserved). Tiling/swizzle rules follow
// Xenia (texture_address.h, xenos.h; BSD-3-Clause).
#include "kernel.h"
#include "settings.h"
#define VK_NO_PROTOTYPES
#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  define VK_USE_PLATFORM_WIN32_KHR
#  include <windows.h>
#endif
#include "volk.h"
#include <atomic>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <string>
#ifndef _WIN32
#  include <SDL2/SDL.h>
#  include <SDL2/SDL_vulkan.h>
#endif

// Shared with gpu_vk.cpp (the draw renderer) once the device exists.
extern std::atomic<bool> g_captureRequest;
struct VkShared { VkInstance inst; VkPhysicalDevice phys; VkDevice dev; VkQueue queue; uint32_t qfam; std::mutex* queueLock;
                  bool geometryShader, bc, aniso, dynamicRendering; float maxAniso; uint32_t apiVersion; };
namespace {
VkShared g_shared{}; bool g_sharedReady = false, g_sharedFailed = false;
std::mutex g_sharedLock, g_queueLock; std::condition_variable g_sharedCv;
struct RenderedFrame { VkImage img = VK_NULL_HANDLE; uint32_t w = 0, h = 0; uint64_t seq = 0; };
RenderedFrame g_rendered;   // guarded by g_frameLock
// ---------------------------------------------------------------- frames handed over by the command processor
struct SwapFrame { uint32_t fetch[6]; uint32_t phys, width, height; };
std::mutex g_frameLock; std::condition_variable g_frameCv;
SwapFrame g_frame{}; uint64_t g_frameSeq = 0;
std::atomic<bool> g_running{false};
bool g_testPattern = false;

int32_t Tiled2D(int32_t x, int32_t y, uint32_t pitchAligned, unsigned bppLog2) {
    int32_t outer = (((y >> 5) * int32_t(pitchAligned >> 5)) + (x >> 5)) << 6;
    int32_t inner = (((y >> 1) & 7) << 3) | (x & 7);
    int32_t oib = (outer | inner) << bppLog2;
    uint32_t bank = (y >> 4) & 1, pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
    return int32_t(((y & 1) << 4) | (pipe << 6) | (bank << 11)) | (oib & 0xF) | (((oib >> 4) & 1) << 5) | (((oib >> 5) & 7) << 8) | (oib >> 8 << 12);
}
uint32_t GpuSwap32(uint32_t v, uint32_t e) {
    switch (e & 3) {
    case 1: return ((v << 8) & 0xFF00FF00) | ((v >> 8) & 0x00FF00FF);
    case 2: return __builtin_bswap32(v);
    case 3: return (v >> 16) | (v << 16);
    default: return v;
    }
}
// Convert the guest front buffer to tightly packed RGBA8.
void ConvertFrontBuffer(const SwapFrame& f, uint8_t* out) {
    uint32_t d0 = f.fetch[0], d1 = f.fetch[1], d3 = f.fetch[3];
    bool tiled = d0 >> 31;
    uint32_t pitch = ((d0 >> 22) & 0x1FF) << 5; if (pitch < f.width) pitch = (f.width + 31) & ~31u;
    uint32_t format = d1 & 0x3F, endian = (d1 >> 6) & 3;
    uint32_t sw = (d3 >> 1) & 0xFFF;
    const uint8_t* src = g_base + GuestFromPhysical(f.phys);
    for (uint32_t y = 0; y < f.height; y++) {
        uint8_t* row = out + (size_t)y * f.width * 4;
        for (uint32_t x = 0; x < f.width; x++) {
            uint32_t off = tiled ? (uint32_t)Tiled2D((int32_t)x, (int32_t)y, pitch, 2) : (y * pitch + x) * 4;
            uint32_t v; memcpy(&v, src + off, 4);           // raw memory order
            v = GpuSwap32(v, endian);
            uint32_t c[4];
            if (format == 7 || format == 54) {              // 2_10_10_10: keep the top 8 bits of each 10-bit channel
                c[0] = (v >> 2) & 0xFF; c[1] = (v >> 12) & 0xFF; c[2] = (v >> 22) & 0xFF; c[3] = (v >> 30) * 85;
            } else {                                        // 8_8_8_8
                c[0] = v & 0xFF; c[1] = (v >> 8) & 0xFF; c[2] = (v >> 16) & 0xFF; c[3] = v >> 24;
            }
            for (int i = 0; i < 4; i++) {
                uint32_t s = (sw >> (3 * i)) & 7;
                row[x * 4 + i] = (uint8_t)(s < 4 ? c[s] : (s == 5 ? 255 : 0));
            }
            row[x * 4 + 3] = 255;                           // the front buffer is shown opaque
        }
    }
    if (g_testPattern) {   // diagnostic: colour bars + moving stripe, proves the presentation path end to end
        static uint32_t t = 0; t += 4;
        const uint8_t bars[8][3] = {{255,255,255},{255,255,0},{0,255,255},{0,255,0},{255,0,255},{255,0,0},{0,0,255},{20,20,20}};
        for (uint32_t y = 0; y < f.height; y++) for (uint32_t x = 0; x < f.width; x++) {
            uint8_t* p = out + ((size_t)y * f.width + x) * 4;
            const uint8_t* b = bars[x * 8 / f.width];
            bool stripe = ((x + t) % f.width) < 24;
            p[0] = stripe ? 0 : b[0]; p[1] = stripe ? 0 : b[1]; p[2] = stripe ? 0 : b[2]; p[3] = 255;
            if (y > f.height * 3 / 4) { uint8_t g = (uint8_t)(x * 255 / f.width); p[0] = p[1] = p[2] = g; }
        }
    }
}

// ---------------------------------------------------------------- window
struct Window {
    uint32_t w = 1600, h = 900; bool closed = false, resized = false, fullscreen = false;
#ifdef _WIN32
    HWND hwnd = nullptr; RECT windowedRect{};
    static LRESULT CALLBACK Proc(HWND hw, UINT m, WPARAM wp, LPARAM lp) {
        auto* self = (Window*)GetWindowLongPtrW(hw, GWLP_USERDATA);
        switch (m) {
        case WM_CLOSE: if (self) self->closed = true; return 0;
        case WM_SIZE: if (self && LOWORD(lp) && HIWORD(lp)) { self->w = LOWORD(lp); self->h = HIWORD(lp); self->resized = true; } return 0;
        case WM_SYSKEYDOWN: if (wp == VK_RETURN && self) { self->ToggleFullscreen(); return 0; } break;
        case WM_KEYDOWN:
            if (wp == VK_F11 && self) { self->ToggleFullscreen(); return 0; }
            if (wp == VK_F12) { g_captureRequest = true; return 0; }
            break;
        }
        return DefWindowProcW(hw, m, wp, lp);
    }
    bool Create(uint32_t cw, uint32_t ch, bool fs) {
        WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"MaddenArcadePC";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        RegisterClassW(&wc);
        RECT r{0, 0, (LONG)cw, (LONG)ch}; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd = CreateWindowW(wc.lpszClassName, L"Madden NFL Arcade PC", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd) return false;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)this);
        w = cw; h = ch;
        ShowWindow(hwnd, SW_SHOW); SetForegroundWindow(hwnd);
        if (fs) ToggleFullscreen();
        return true;
    }
    void ToggleFullscreen() {   // borderless fullscreen on the window's monitor
        if (!fullscreen) {
            GetWindowRect(hwnd, &windowedRect);
            MONITORINFO mi{sizeof mi}; GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            SetWindowLongW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
        } else {
            SetWindowLongW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
            SetWindowPos(hwnd, nullptr, windowedRect.left, windowedRect.top, windowedRect.right - windowedRect.left,
                         windowedRect.bottom - windowedRect.top, SWP_FRAMECHANGED | SWP_NOZORDER);
        }
        fullscreen = !fullscreen; resized = true;
    }
    void Pump() { MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
    void SetTitle(const std::string& t) { std::wstring ws(t.begin(), t.end()); SetWindowTextW(hwnd, ws.c_str()); }
    std::vector<const char*> InstanceExtensions() { return {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME}; }
    bool CreateSurface(VkInstance inst, VkSurfaceKHR* s) {
        VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR}; ci.hinstance = GetModuleHandleW(nullptr); ci.hwnd = hwnd;
        return vkCreateWin32SurfaceKHR(inst, &ci, nullptr, s) == VK_SUCCESS;
    }
#else
    SDL_Window* win = nullptr;
    bool Create(uint32_t cw, uint32_t ch, bool fs) {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "[video] SDL: %s\n", SDL_GetError()); return false; }
        win = SDL_CreateWindow("Madden NFL Arcade PC", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, (int)cw, (int)ch,
                               SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | (fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
        if (!win) { fprintf(stderr, "[video] SDL window: %s\n", SDL_GetError()); return false; }
        int dw, dh; SDL_Vulkan_GetDrawableSize(win, &dw, &dh); w = dw; h = dh; fullscreen = fs;
        return true;
    }
    void ToggleFullscreen() { fullscreen = !fullscreen; SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0); resized = true; }
    void Pump() {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) closed = true;
            else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) resized = true;
            else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F12) g_captureRequest = true;
            else if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_F11 || (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT)))) ToggleFullscreen();
        }
        if (resized) { int dw, dh; SDL_Vulkan_GetDrawableSize(win, &dw, &dh); if (dw && dh) { w = dw; h = dh; } }
    }
    void SetTitle(const std::string& t) { SDL_SetWindowTitle(win, t.c_str()); }
    std::vector<const char*> InstanceExtensions() {
        unsigned n = 0; SDL_Vulkan_GetInstanceExtensions(win, &n, nullptr);
        std::vector<const char*> v(n); SDL_Vulkan_GetInstanceExtensions(win, &n, v.data()); return v;
    }
    bool CreateSurface(VkInstance inst, VkSurfaceKHR* s) { return SDL_Vulkan_CreateSurface(win, inst, s) == SDL_TRUE; }
#endif
};

// ---------------------------------------------------------------- Vulkan presenter
struct VkPresenter {
    Window win;
    VkInstance inst = VK_NULL_HANDLE; VkSurfaceKHR surface = VK_NULL_HANDLE; VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE; VkQueue queue = VK_NULL_HANDLE; uint32_t qfam = 0;
    VkSwapchainKHR swap = VK_NULL_HANDLE; VkFormat swapFormat = VK_FORMAT_B8G8R8A8_UNORM; VkExtent2D extent{};
    std::vector<VkImage> swapImages;
    VkCommandPool pool = VK_NULL_HANDLE; VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE;
    VkImage frameImage = VK_NULL_HANDLE; VkDeviceMemory frameMem = VK_NULL_HANDLE; uint32_t frameW = 0, frameH = 0;
    VkBuffer staging = VK_NULL_HANDLE; VkDeviceMemory stagingMem = VK_NULL_HANDLE; void* stagingPtr = nullptr; size_t stagingSize = 0;
    bool canBlit = true; std::string gpuName;
    // high-quality downscale: the internal image (up to 4K) is halved step by step (each 2:1 linear blit averages
    // 2x2 pixels) until one more halving would go below the window size, then scaled once. A single large-ratio
    // linear blit only samples a few source pixels and threw most of the 4K detail away (crunchy small text).
    struct Inter { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; uint32_t w = 0, h = 0; };
    std::vector<Inter> chain;
    void EnsureChain(uint32_t w, uint32_t h, uint32_t tw, uint32_t th) {
        std::vector<std::pair<uint32_t, uint32_t>> want;
        while (w / 2 >= tw && h / 2 >= th && w >= 4 && h >= 4) { w /= 2; h /= 2; want.push_back({w, h}); }
        bool same = want.size() == chain.size();
        for (size_t i = 0; same && i < want.size(); i++) same = chain[i].w == want[i].first && chain[i].h == want[i].second;
        if (same) return;
        vkDeviceWaitIdle(dev);
        for (auto& c : chain) { vkDestroyImage(dev, c.img, nullptr); vkFreeMemory(dev, c.mem, nullptr); }
        chain.clear();
        for (auto [cw, ch] : want) {
            Inter it; it.w = cw; it.h = ch;
            VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_R8G8B8A8_UNORM;
            ci.extent = {cw, ch, 1}; ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
            ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(dev, &ci, nullptr, &it.img) != VK_SUCCESS) break;
            VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, it.img, &mr);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size; ai.memoryTypeIndex = MemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (vkAllocateMemory(dev, &ai, nullptr, &it.mem) != VK_SUCCESS) { vkDestroyImage(dev, it.img, nullptr); break; }
            vkBindImageMemory(dev, it.img, it.mem, 0);
            chain.push_back(it);
        }
    }

    uint32_t MemType(uint32_t bits, VkMemoryPropertyFlags want) {
        VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
        return UINT32_MAX;
    }
    bool Init() {
        if (volkInitialize() != VK_SUCCESS) { fprintf(stderr, "[video] Vulkan is not installed on this system (no vulkan-1 loader)\n"); return false; }
        if (!win.Create(g_settings.windowWidth, g_settings.windowHeight, g_settings.fullscreen)) return false;
        auto exts = win.InstanceExtensions();
        uint32_t loaderVer = VK_API_VERSION_1_0; if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&loaderVer);
        VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.pApplicationName = "Madden NFL Arcade PC";
        ai.apiVersion = loaderVer >= VK_API_VERSION_1_3 ? VK_API_VERSION_1_3 : VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &ai;
        ici.enabledExtensionCount = (uint32_t)exts.size(); ici.ppEnabledExtensionNames = exts.data();
        if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) { fprintf(stderr, "[video] vkCreateInstance failed\n"); return false; }
        volkLoadInstance(inst);
        if (!win.CreateSurface(inst, &surface)) { fprintf(stderr, "[video] could not create a Vulkan surface\n"); return false; }

        uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, nullptr);
        std::vector<VkPhysicalDevice> pds(n); vkEnumeratePhysicalDevices(inst, &n, pds.data());
        int best = -1, bestScore = -1;
        for (uint32_t i = 0; i < n; i++) {
            VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p);
            uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qs(qn); vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &qn, qs.data());
            int fam = -1;
            for (uint32_t q = 0; q < qn; q++) { VkBool32 pres = 0; vkGetPhysicalDeviceSurfaceSupportKHR(pds[i], q, surface, &pres); if ((qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && pres) { fam = (int)q; break; } }
            if (fam < 0) continue;
            int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
            if (!g_settings.gpu.empty() && g_settings.gpu == p.deviceName) score = 100;
            fprintf(stderr, "[video] GPU %u: %s (score %d)\n", i, p.deviceName, score);
            if (score > bestScore) { bestScore = score; best = (int)i; qfam = (uint32_t)fam; }
        }
        if (best < 0) { fprintf(stderr, "[video] no Vulkan GPU can present to this window\n"); return false; }
        phys = pds[best];
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(phys, &p); gpuName = p.deviceName;
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex = qfam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
        std::vector<const char*> dexts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkPhysicalDeviceFeatures feats{}; VkPhysicalDeviceFeatures have; vkGetPhysicalDeviceFeatures(phys, &have);
        feats.samplerAnisotropy = have.samplerAnisotropy; feats.geometryShader = have.geometryShader; feats.robustBufferAccess = have.robustBufferAccess;
        feats.textureCompressionBC = have.textureCompressionBC; feats.independentBlend = have.independentBlend;
        feats.largePoints = have.largePoints; feats.shaderTessellationAndGeometryPointSize = have.shaderTessellationAndGeometryPointSize;
        // dynamic rendering (core in 1.3, else VK_KHR_dynamic_rendering)
        bool dynRender = false;
        uint32_t devApi = std::min(p.apiVersion, ai.apiVersion);
        VkPhysicalDeviceDynamicRenderingFeatures drf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
        {
            uint32_t en = 0; vkEnumerateDeviceExtensionProperties(phys, nullptr, &en, nullptr);
            std::vector<VkExtensionProperties> ex(en); vkEnumerateDeviceExtensionProperties(phys, nullptr, &en, ex.data());
            bool hasExt = false; for (auto& e : ex) if (!strcmp(e.extensionName, "VK_KHR_dynamic_rendering")) hasExt = true;
            if (devApi >= VK_API_VERSION_1_3 || hasExt) {
                VkPhysicalDeviceDynamicRenderingFeatures q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
                VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; f2.pNext = &q;
                if (vkGetPhysicalDeviceFeatures2) vkGetPhysicalDeviceFeatures2(phys, &f2);
                else if (vkGetPhysicalDeviceFeatures2KHR) vkGetPhysicalDeviceFeatures2KHR(phys, &f2);
                dynRender = q.dynamicRendering;
                if (dynRender && devApi < VK_API_VERSION_1_3) dexts.push_back("VK_KHR_dynamic_rendering");
            }
        }
        drf.dynamicRendering = dynRender;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = (uint32_t)dexts.size(); dci.ppEnabledExtensionNames = dexts.data(); dci.pEnabledFeatures = &feats;
        if (dynRender) dci.pNext = &drf;
        if (vkCreateDevice(phys, &dci, nullptr, &dev) != VK_SUCCESS) { fprintf(stderr, "[video] vkCreateDevice failed\n"); return false; }
        volkLoadDevice(dev);
        vkGetDeviceQueue(dev, qfam, 0, &queue);
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = qfam;
        vkCreateCommandPool(dev, &pci, nullptr, &pool);
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        vkAllocateCommandBuffers(dev, &cai, &cmd);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(dev, &sci, nullptr, &acquired); vkCreateSemaphore(dev, &sci, nullptr, &rendered);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fci.flags = VK_FENCE_CREATE_SIGNALED_BIT; vkCreateFence(dev, &fci, nullptr, &fence);
        if (!CreateSwapchain()) return false;
        {
            std::lock_guard<std::mutex> l(g_sharedLock);
            g_shared = {inst, phys, dev, queue, qfam, &g_queueLock, (bool)feats.geometryShader, (bool)feats.textureCompressionBC,
                        (bool)feats.samplerAnisotropy, dynRender, p.limits.maxSamplerAnisotropy, devApi};
            g_sharedReady = true; g_sharedCv.notify_all();
        }
        fprintf(stderr, "[video] Vulkan renderer on %s (Vulkan %u.%u), internal resolution %ux%u (%ux), window %ux%u%s\n", gpuName.c_str(),
                VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), 1280 * g_settings.resolutionScale, 720 * g_settings.resolutionScale,
                g_settings.resolutionScale, win.w, win.h, win.fullscreen ? " (fullscreen)" : "");
        return true;
    }
    bool CreateSwapchain() {
        vkDeviceWaitIdle(dev);
        VkSurfaceCapabilitiesKHR caps; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
        uint32_t fn = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fn, nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(fn); vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fn, fmts.data());
        VkSurfaceFormatKHR fmt = fmts[0];
        for (auto& f : fmts) if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) { fmt = f; break; }
        swapFormat = fmt.format;
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(phys, swapFormat, &fp);
        canBlit = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
        uint32_t pn = 0; vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &pn, nullptr);
        std::vector<VkPresentModeKHR> modes(pn); vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &pn, modes.data());
        VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
        if (!g_settings.vsync) for (auto m : modes) if (m == VK_PRESENT_MODE_MAILBOX_KHR || m == VK_PRESENT_MODE_IMMEDIATE_KHR) { mode = m; if (m == VK_PRESENT_MODE_MAILBOX_KHR) break; }
        extent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : VkExtent2D{win.w, win.h};
        if (!extent.width || !extent.height) return false;
        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface = surface; ci.minImageCount = std::max(caps.minImageCount, 2u); if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount) ci.minImageCount = caps.maxImageCount;
        ci.imageFormat = fmt.format; ci.imageColorSpace = fmt.colorSpace; ci.imageExtent = extent; ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = caps.currentTransform; ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; ci.presentMode = mode; ci.clipped = VK_TRUE;
        ci.oldSwapchain = swap;
        VkSwapchainKHR ns; if (vkCreateSwapchainKHR(dev, &ci, nullptr, &ns) != VK_SUCCESS) { fprintf(stderr, "[video] swapchain creation failed\n"); return false; }
        if (swap) vkDestroySwapchainKHR(dev, swap, nullptr);
        swap = ns;
        uint32_t in = 0; vkGetSwapchainImagesKHR(dev, swap, &in, nullptr); swapImages.resize(in); vkGetSwapchainImagesKHR(dev, swap, &in, swapImages.data());
        return true;
    }
    void EnsureFrameResources(uint32_t w, uint32_t h) {
        if (w == frameW && h == frameH && frameImage) return;
        vkDeviceWaitIdle(dev);
        if (frameImage) { vkDestroyImage(dev, frameImage, nullptr); vkFreeMemory(dev, frameMem, nullptr); }
        if (staging) { vkUnmapMemory(dev, stagingMem); vkDestroyBuffer(dev, staging, nullptr); vkFreeMemory(dev, stagingMem, nullptr); }
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {w, h, 1}; ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        vkCreateImage(dev, &ici, nullptr, &frameImage);
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, frameImage, &mr);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize = mr.size; mai.memoryTypeIndex = MemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(dev, &mai, nullptr, &frameMem); vkBindImageMemory(dev, frameImage, frameMem, 0);
        stagingSize = (size_t)w * h * 4;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bci.size = stagingSize; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        vkCreateBuffer(dev, &bci, nullptr, &staging);
        vkGetBufferMemoryRequirements(dev, staging, &mr);
        mai.allocationSize = mr.size; mai.memoryTypeIndex = MemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(dev, &mai, nullptr, &stagingMem); vkBindBufferMemory(dev, staging, stagingMem, 0);
        vkMapMemory(dev, stagingMem, 0, stagingSize, 0, &stagingPtr);
        frameW = w; frameH = h;
    }
    static void Barrier(VkCommandBuffer c, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.oldLayout = from; b.newLayout = to; b.srcAccessMask = sa; b.dstAccessMask = da;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(c, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    void Present(const SwapFrame* f, const RenderedFrame* rf) {
        if (win.resized) { win.resized = false; if (!CreateSwapchain()) return; }
        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        uint32_t idx;
        VkResult r = vkAcquireNextImageKHR(dev, swap, UINT64_MAX, acquired, VK_NULL_HANDLE, &idx);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) { CreateSwapchain(); return; }
        vkResetFences(dev, 1, &fence);
        if (rf && rf->img) f = nullptr;   // the GPU renderer produced this frame
        if (f) { EnsureFrameResources(f->width, f->height); ConvertFrontBuffer(*f, (uint8_t*)stagingPtr); }
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        VkImage dst = swapImages[idx];
        Barrier(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue black{}; VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rng);
        if (f && canBlit) {
            Barrier(cmd, frameImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy bc{}; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bc.imageExtent = {frameW, frameH, 1};
            vkCmdCopyBufferToImage(cmd, staging, frameImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
            Barrier(cmd, frameImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            // aspect-preserving fit
            double s = std::min((double)extent.width / frameW, (double)extent.height / frameH);
            int32_t ow = (int32_t)(frameW * s), oh = (int32_t)(frameH * s), ox = ((int32_t)extent.width - ow) / 2, oy = ((int32_t)extent.height - oh) / 2;
            VkImageBlit bl{}; bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bl.srcOffsets[1] = {(int32_t)frameW, (int32_t)frameH, 1};
            bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bl.dstOffsets[0] = {ox, oy, 0}; bl.dstOffsets[1] = {ox + ow, oy + oh, 1};
            vkCmdBlitImage(cmd, frameImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
        }
        if (rf && rf->img && canBlit) {
            double s = std::min((double)extent.width / rf->w, (double)extent.height / rf->h);
            int32_t ow = (int32_t)(rf->w * s), oh = (int32_t)(rf->h * s), ox = ((int32_t)extent.width - ow) / 2, oy = ((int32_t)extent.height - oh) / 2;
            EnsureChain(rf->w, rf->h, (uint32_t)std::max(ow, 1), (uint32_t)std::max(oh, 1));
            VkImage src = rf->img; VkImageLayout srcLayout = VK_IMAGE_LAYOUT_GENERAL; int32_t sw = (int32_t)rf->w, sh = (int32_t)rf->h;
            for (auto& it : chain) {
                Barrier(cmd, it.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkImageBlit hb{}; hb.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; hb.srcOffsets[1] = {sw, sh, 1};
                hb.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; hb.dstOffsets[1] = {(int32_t)it.w, (int32_t)it.h, 1};
                vkCmdBlitImage(cmd, src, srcLayout, it.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &hb, VK_FILTER_LINEAR);
                Barrier(cmd, it.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                src = it.img; srcLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; sw = (int32_t)it.w; sh = (int32_t)it.h;
            }
            VkImageBlit bl{}; bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bl.srcOffsets[1] = {sw, sh, 1};
            bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bl.dstOffsets[0] = {ox, oy, 0}; bl.dstOffsets[1] = {ox + ow, oy + oh, 1};
            vkCmdBlitImage(cmd, src, srcLayout, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
        }
        Barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        vkEndCommandBuffer(cmd);
        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.waitSemaphoreCount = 1; si.pWaitSemaphores = &acquired; si.pWaitDstStageMask = &ws;
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd; si.signalSemaphoreCount = 1; si.pSignalSemaphores = &rendered;
        std::lock_guard<std::mutex> ql(g_queueLock);
        vkQueueSubmit(queue, 1, &si, fence);
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &rendered;
        pi.swapchainCount = 1; pi.pSwapchains = &swap; pi.pImageIndices = &idx;
        r = vkQueuePresentKHR(queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) win.resized = true;
    }
};

void RenderThread() {
    VkPresenter vp;
    if (!vp.Init()) {
        fprintf(stderr, "[video] no window - the game keeps running headless\n"); g_running = false;
        std::lock_guard<std::mutex> l(g_sharedLock); g_sharedFailed = true; g_sharedCv.notify_all(); return;
    }
    uint64_t lastSeq = 0; SwapFrame frame{}; bool haveFrame = false; RenderedFrame rframe{};
    uint32_t swaps = 0; auto tick = std::chrono::steady_clock::now(); auto lastPresent = tick; bool fresh = false;
    auto title = [&](double fps) {
        char t[256];
        snprintf(t, sizeof t, "Madden NFL Arcade PC  |  Vulkan - %s  |  internal %ux%u (%ux)%s%s", vp.gpuName.c_str(),
                 1280 * g_settings.resolutionScale, 720 * g_settings.resolutionScale, g_settings.resolutionScale,
                 g_settings.showFps ? "  |  " : "", g_settings.showFps ? (std::to_string((int)(fps + 0.5)) + " fps").c_str() : "");
        vp.win.SetTitle(t);
    };
    title(0);
    while (g_running) {
        vp.win.Pump();
        if (vp.win.closed) { fprintf(stderr, "[video] window closed - exiting\n"); DumpGuestThreads(); fflush(stderr); _Exit(0); }
        {
            std::unique_lock<std::mutex> l(g_frameLock);
            // Present once per new guest frame (not on a fixed 16 ms timer): a fixed timer re-presented stale
            // frames and made new ones wait an extra refresh, which showed up as judder.
            fresh = g_frameCv.wait_for(l, std::chrono::milliseconds(8), [&] { return g_frameSeq != lastSeq; });
            if (fresh) { swaps += (uint32_t)(g_frameSeq - lastSeq); lastSeq = g_frameSeq; frame = g_frame; haveFrame = true; rframe = g_rendered; }
        }
        auto nowP = std::chrono::steady_clock::now();
        if (fresh || vp.win.resized || nowP - lastPresent > std::chrono::milliseconds(250)) {
            vp.Present(haveFrame ? &frame : nullptr, rframe.img ? &rframe : nullptr);
            lastPresent = nowP;
        }
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - tick).count();
        if (dt >= 1.0) { title(swaps / dt); swaps = 0; tick = now; }
    }
}
}   // namespace

// Called by the command processor on XE_SWAP.
void PresenterSubmitSwap(const uint32_t fetch[6], uint32_t phys, uint32_t width, uint32_t height) {
    if (!g_running) return;
    std::lock_guard<std::mutex> l(g_frameLock);
    memcpy(g_frame.fetch, fetch, sizeof g_frame.fetch);
    g_frame.phys = phys & 0x1FFFFFFF; g_frame.width = std::min(width, 4096u); g_frame.height = std::min(height, 4096u);
    g_frameSeq++;
    g_frameCv.notify_one();
}
// The draw renderer waits here for the presenter's device (nullptr = no Vulkan).
const VkShared* PresenterWaitVulkan() {
    if (!g_running) return nullptr;
    std::unique_lock<std::mutex> l(g_sharedLock);
    g_sharedCv.wait(l, [] { return g_sharedReady || g_sharedFailed; });
    return g_sharedReady ? &g_shared : nullptr;
}
// The draw renderer hands over a finished frame (image in GENERAL layout, rendering already complete).
void PresenterSubmitRendered(VkImage img, uint32_t w, uint32_t h) {
    std::lock_guard<std::mutex> l(g_frameLock);
    g_rendered.img = img; g_rendered.w = w; g_rendered.h = h; g_rendered.seq++;
}
void PresenterSubmitRenderedClear() { std::lock_guard<std::mutex> l(g_frameLock); g_rendered = {}; }
void PresenterStart() {
    g_testPattern = getenv("MADDEN_TEST_PATTERN") != nullptr;
    if (g_settings.renderer == "none") { fprintf(stderr, "[video] renderer disabled (headless)\n"); return; }
    if (g_settings.renderer == "d3d12") fprintf(stderr, "[video] the Direct3D 12 renderer is not available yet - using Vulkan\n");
    g_running = true;
    std::thread(RenderThread).detach();
}
