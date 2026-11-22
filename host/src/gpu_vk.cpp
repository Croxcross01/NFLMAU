// Vulkan backend for Xenos draws.
// Register meanings, resolve rules and texture layouts follow Xenia (BSD-3-Clause, Copyright Ben Vanik and
// Xenia contributors); the implementation here is an independent, simplified "host render target" design:
//  - every EDRAM render target becomes a Vulkan image (scaled by the internal resolution setting),
//  - resolves copy into GPU images keyed by the destination address and are sampled from there directly,
//  - other textures are decoded from guest memory (mip 0) and cached by content hash,
//  - vertex data is copied per draw into an upload ring and fetched in the shader (endian aware).
#include "kernel.h"
#include "settings.h"
#include "gpu_vk.h"
#include "xenos_shader.h"
#define VK_NO_PROTOTYPES
#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  define VK_USE_PLATFORM_WIN32_KHR
#  include <windows.h>
#endif
#include "volk.h"
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct VkShared { VkInstance inst; VkPhysicalDevice phys; VkDevice dev; VkQueue queue; uint32_t qfam; std::mutex* queueLock;
                  bool geometryShader, bc, aniso, dynamicRendering; float maxAniso; uint32_t apiVersion; };
const VkShared* PresenterWaitVulkan();
std::atomic<bool> g_captureRequest{false};   // set by the window on F12
void PresenterSubmitRendered(VkImage img, uint32_t w, uint32_t h);

namespace {

// ---------------------------------------------------------------- registers
enum : uint32_t {
    R_SURFACE_INFO = 0x2000, R_COLOR_INFO = 0x2001, R_DEPTH_INFO = 0x2002, R_COLOR1_INFO = 0x2003,
    R_WINDOW_OFFSET = 0x2080, R_WINDOW_SCISSOR_TL = 0x2081, R_WINDOW_SCISSOR_BR = 0x2082,
    R_INDX_OFFSET = 0x2102, R_RESET_INDX = 0x2103, R_COLOR_MASK = 0x2104, R_BLEND_RED = 0x2105,
    R_STENCILREFMASK_BF = 0x210C, R_STENCILREFMASK = 0x210D, R_ALPHA_REF = 0x210E,
    R_VPORT_XSCALE = 0x210F, R_SQ_PROGRAM_CNTL = 0x2180, R_SQ_CONTEXT_MISC = 0x2181,
    R_DEPTHCONTROL = 0x2200, R_BLENDCONTROL0 = 0x2201, R_COLORCONTROL = 0x2202, R_SC_MODE_CNTL = 0x2205,
    R_VTE_CNTL = 0x2206, R_MODECONTROL = 0x2208, R_BLENDCONTROL1 = 0x2209, R_VTX_CNTL = 0x2302,
    R_COPY_CONTROL = 0x2318, R_COPY_DEST_BASE = 0x2319, R_COPY_DEST_PITCH = 0x231A, R_COPY_DEST_INFO = 0x231B,
    R_DEPTH_CLEAR = 0x231D, R_COLOR_CLEAR = 0x231E, R_COLOR_CLEAR_LO = 0x231F,
    R_CONST_FLOAT = 0x4000, R_CONST_FETCH = 0x4800, R_CONST_BOOL = 0x4900, R_CONST_LOOP = 0x4908,
};
uint32_t Reg(uint32_t i) { return GpuRegister(i); }
float RegF(uint32_t i) { uint32_t v = Reg(i); float f; memcpy(&f, &v, 4); return f; }

int32_t Tiled2D(int32_t x, int32_t y, uint32_t pitchAligned, unsigned bppLog2) {
    int32_t outer = (((y >> 5) * int32_t(pitchAligned >> 5)) + (x >> 5)) << 6;
    int32_t inner = (((y >> 1) & 7) << 3) | (x & 7);
    int32_t oib = (outer | inner) << bppLog2;
    uint32_t bank = (y >> 4) & 1, pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
    return int32_t(((y & 1) << 4) | (pipe << 6) | (bank << 11)) | (oib & 0xF) | (((oib >> 4) & 1) << 5) | (((oib >> 5) & 7) << 8) | (oib >> 8 << 12);
}
uint64_t Fnv64(const void* data, size_t n, uint64_t h = 1469598103934665603ull) {
    const uint8_t* p = (const uint8_t*)data;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t v; memcpy(&v, p + i, 8); h ^= v; h *= 1099511628211ull; h ^= h >> 29; }
    for (; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// ---------------------------------------------------------------- state
const VkShared* V = nullptr;
bool g_active = false, g_tried = false;
uint32_t S = 1;   // internal resolution scale
VkPhysicalDeviceProperties g_props{};
VkFormat g_depthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
uint64_t g_frameNumber = 0;
bool g_log = getenv("MADDEN_RENDER_LOG") != nullptr;
uint64_t g_traceFrame = getenv("MADDEN_TRACE_FRAME") ? strtoull(getenv("MADDEN_TRACE_FRAME"), nullptr, 10) : ~0ull;
bool Tracing();
FILE* g_traceOut = stderr;
bool g_capturing = false; std::string g_captureDir; int g_captureSeq = 0;
std::vector<uint64_t> g_captureShaders;
#define TRACE_OUT g_traceOut

uint32_t MemType(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(V->phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

struct Image {
    VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE;
    VkFormat fmt = VK_FORMAT_UNDEFINED; uint32_t w = 0, h = 0; bool depth = false; VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
    uint32_t layers = 1, mips = 1;
    std::map<uint32_t, VkImageView> swizzled;   // sampled views by fetch-constant swizzle
    void Destroy() {
        for (auto& [k, v] : swizzled) vkDestroyImageView(V->dev, v, nullptr);
        swizzled.clear();
        if (view) vkDestroyImageView(V->dev, view, nullptr);
        if (img) vkDestroyImage(V->dev, img, nullptr);
        if (mem) vkFreeMemory(V->dev, mem, nullptr);
        img = VK_NULL_HANDLE; mem = VK_NULL_HANDLE; view = VK_NULL_HANDLE;
    }
};
std::vector<Image*> g_deferredDestroy[2];

VkFormat SrgbOf(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case VK_FORMAT_BC2_UNORM_BLOCK: return VK_FORMAT_BC2_SRGB_BLOCK;
    case VK_FORMAT_BC3_UNORM_BLOCK: return VK_FORMAT_BC3_SRGB_BLOCK;
    default: return VK_FORMAT_UNDEFINED;
    }
}
bool CreateImage(Image& im, VkFormat fmt, uint32_t w, uint32_t h, bool depth, VkImageUsageFlags usage, uint32_t layers = 1, bool cube = false, bool array = false, uint32_t mips = 1) {
    im.fmt = fmt; im.w = w; im.h = h; im.depth = depth; im.layers = layers; im.mips = std::max(mips, 1u);
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = {w, h, 1}; ci.mipLevels = im.mips; ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (cube) ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (SrgbOf(fmt) != VK_FORMAT_UNDEFINED) ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;   // gamma views
    if (vkCreateImage(V->dev, &ci, nullptr, &im.img) != VK_SUCCESS) return false;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(V->dev, im.img, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size; ai.memoryTypeIndex = MemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(V->dev, &ai, nullptr, &im.mem) != VK_SUCCESS) { vkDestroyImage(V->dev, im.img, nullptr); im.img = VK_NULL_HANDLE; return false; }
    vkBindImageMemory(V->dev, im.img, im.mem, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = im.img;
    vi.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange = {depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT), 0, im.mips, 0, layers};
    im.viewType = vi.viewType;
    vkCreateImageView(V->dev, &vi, nullptr, &im.view);
    return true;
}
// swz: Xenos fetch swizzle (3 bits per component). srgb: decode gamma on sampling. swapRB: data was resolved with red/blue swapped.
bool CreateImage3D(Image& im, VkFormat fmt, uint32_t w, uint32_t h, uint32_t d) {
    im.fmt = fmt; im.w = w; im.h = h; im.depth = false;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_3D; ci.format = fmt; ci.extent = {w, h, d}; ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (SrgbOf(fmt) != VK_FORMAT_UNDEFINED) ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if (vkCreateImage(V->dev, &ci, nullptr, &im.img) != VK_SUCCESS) return false;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(V->dev, im.img, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size; ai.memoryTypeIndex = MemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(V->dev, &ai, nullptr, &im.mem) != VK_SUCCESS) { vkDestroyImage(V->dev, im.img, nullptr); im.img = VK_NULL_HANDLE; return false; }
    vkBindImageMemory(V->dev, im.img, im.mem, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = im.img; vi.viewType = VK_IMAGE_VIEW_TYPE_3D; vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    im.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vkCreateImageView(V->dev, &vi, nullptr, &im.view);
    return true;
}
// Xenos 3D tiling (Xenia texture_address.h, BSD-3-Clause): 32x16x4-block macro tiles.
int64_t Tiled3D(int32_t x, int32_t y, int32_t z, uint32_t pitchAligned, uint32_t heightAligned, unsigned bppLog2) {
    int32_t outer = ((((z >> 2) * int32_t(heightAligned >> 4) + (y >> 4)) * int32_t(pitchAligned >> 5)) + (x >> 5)) << 7;
    int32_t inner = ((z & 3) << 5) | (((y >> 1) & 3) << 3) | (x & 7);
    int64_t oib = int64_t(outer | inner) << bppLog2;
    uint32_t bank = ((y >> 3) ^ (z >> 2)) & 1, pipe = ((x >> 3) & 3) ^ (bank << 1);
    return int64_t(((y & 1) << 4) | (pipe << 6) | (bank << 11)) | (oib & 0xF) | (((oib >> 4) & 1) << 5) | (((oib >> 5) & 7) << 8) | (oib >> 8 << 12);
}
VkImageView SwizzledView(Image& im, uint32_t swz, bool srgb = false, bool swapRB = false) {
    if (swapRB) { uint32_t o = 0; for (int i = 0; i < 4; i++) { uint32_t c = (swz >> (3 * i)) & 7; if (c == 0) c = 2; else if (c == 2) c = 0; o |= c << (3 * i); } swz = o; }
    if (srgb && SrgbOf(im.fmt) == VK_FORMAT_UNDEFINED) srgb = false;
    uint32_t key = swz | (srgb ? 0x10000 : 0);
    auto it = im.swizzled.find(key);
    if (it != im.swizzled.end()) return it->second;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = im.img; vi.viewType = im.viewType; vi.format = srgb ? SrgbOf(im.fmt) : im.fmt;
    VkComponentSwizzle* c[4] = {&vi.components.r, &vi.components.g, &vi.components.b, &vi.components.a};
    // single/dual channel formats replicate like Xenia's host swizzles (RRRR / RGGG)
    int chans = 4;
    switch (im.fmt) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_BC4_UNORM_BLOCK: chans = 1; break;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_BC5_UNORM_BLOCK: case VK_FORMAT_R16G16_SNORM: chans = 2; break;
    default: break;
    }
    for (int i = 0; i < 4; i++) {
        uint32_t s = (swz >> (3 * i)) & 7;
        if (s < 4 && (int)s >= chans) s = (uint32_t)chans - 1;
        *c[i] = s == 0 ? VK_COMPONENT_SWIZZLE_R : s == 1 ? VK_COMPONENT_SWIZZLE_G : s == 2 ? VK_COMPONENT_SWIZZLE_B : s == 3 ? VK_COMPONENT_SWIZZLE_A :
                s == 4 ? VK_COMPONENT_SWIZZLE_ZERO : VK_COMPONENT_SWIZZLE_ONE;
    }
    vi.subresourceRange = {im.depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT), 0, im.mips, 0, im.layers};
    VkImageView v = VK_NULL_HANDLE; vkCreateImageView(V->dev, &vi, nullptr, &v);
    im.swizzled[key] = v;
    return v;
}

// ---------------------------------------------------------------- per-frame resources
struct Ring { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; uint8_t* ptr = nullptr; size_t size = 0, used = 0; };
struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE; VkCommandBuffer cmd = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE;
    Ring ring; VkDescriptorPool dpool = VK_NULL_HANDLE; bool recording = false;
    std::unordered_map<uint64_t, uint32_t> vbCache;   // (addr, size) -> ring dword offset
};
Frame g_frames[2]; int g_cur = 0;
constexpr size_t kRingSize = 96u << 20;

bool CreateRing(Ring& r, size_t size) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = size;
    bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(V->dev, &bi, nullptr, &r.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(V->dev, r.buf, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size;
    ai.memoryTypeIndex = MemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(V->dev, &ai, nullptr, &r.mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(V->dev, r.buf, r.mem, 0);
    vkMapMemory(V->dev, r.mem, 0, size, 0, (void**)&r.ptr);
    r.size = size; r.used = 0;
    return true;
}

// ---------------------------------------------------------------- shaders and pipelines
VkDescriptorSetLayout g_setLayout = VK_NULL_HANDLE;
VkPipelineLayout g_pipeLayout = VK_NULL_HANDLE;
constexpr uint32_t kPsTexBase = 8, kPsTexCount = 16, kVsTexBase = 40, kVsTexCount = 8;
constexpr size_t kUboSize = 512 * 16 + 32 + 128 + 768 + 16 * 5 + 24 * 16;
VkShaderModule g_rectGs = VK_NULL_HANDLE, g_blitVs = VK_NULL_HANDLE, g_blitPs = VK_NULL_HANDLE;
VkDescriptorSetLayout g_blitSetLayout = VK_NULL_HANDLE; VkPipelineLayout g_blitLayout = VK_NULL_HANDLE; VkPipeline g_blitPipe = VK_NULL_HANDLE;
VkSampler g_linearClamp = VK_NULL_HANDLE;

struct Shader { xenos::TranslatedShader t; VkShaderModule mod = VK_NULL_HANDLE; bool failed = false; };
std::unordered_map<uint64_t, std::unique_ptr<Shader>> g_shaders;   // by microcode hash
std::unordered_map<uint64_t, std::vector<uint32_t>> g_microcode;   // filled by the command processor

// ---------------------------------------------------------------- persistent shader cache
// shader_cache.bin next to the exe: records of {u64 key, u32 dwordCount, spirv[dwordCount]}.
// The key hashes the GLSL text + stage, so any translator change simply misses and recompiles.
std::string ExeDir();
std::unordered_map<uint64_t, std::vector<uint32_t>> g_spvCache;
FILE* g_spvFile = nullptr;
std::mutex g_spvLock;
constexpr uint32_t kSpvMagic = 0x4D534331;   // 'MSC1'
uint64_t HashText(const std::string& s, uint32_t salt) {
    uint64_t h = 1469598103934665603ull ^ salt;
    for (unsigned char ch : s) { h ^= ch; h *= 1099511628211ull; }
    return h;
}
void LoadShaderCache() {
    std::string path = ExeDir() + "/shader_cache.bin";
    if (FILE* f = fopen(path.c_str(), "rb")) {
        uint32_t magic = 0;
        if (fread(&magic, 4, 1, f) == 1 && magic == kSpvMagic) {
            for (;;) {
                uint64_t key; uint32_t n;
                if (fread(&key, 8, 1, f) != 1 || fread(&n, 4, 1, f) != 1 || n == 0 || n > (1u << 22)) break;
                std::vector<uint32_t> v(n);
                if (fread(v.data(), 4, n, f) != n) break;
                g_spvCache[key] = std::move(v);
            }
        }
        fclose(f);
    }
    bool fresh = g_spvCache.empty();
    g_spvFile = fopen(path.c_str(), fresh ? "wb" : "ab");
    if (g_spvFile && fresh) { fwrite(&kSpvMagic, 4, 1, g_spvFile); fflush(g_spvFile); }
    fprintf(stderr, "[render] shader cache: %zu compiled shaders loaded\n", g_spvCache.size());
}
void StoreShader(uint64_t key, const std::vector<uint32_t>& spv) {
    std::lock_guard<std::mutex> l(g_spvLock);
    g_spvCache[key] = spv;
    if (!g_spvFile) return;
    uint32_t n = uint32_t(spv.size());
    fwrite(&key, 8, 1, g_spvFile); fwrite(&n, 4, 1, g_spvFile); fwrite(spv.data(), 4, n, g_spvFile); fflush(g_spvFile);
}

// pipeline_cache.bin: the driver's own VkPipelineCache blob, saved whenever new pipelines were built.
VkPipelineCache g_pipeCache = VK_NULL_HANDLE;
bool g_pipeCacheDirty = false;
std::chrono::steady_clock::time_point g_pipeCacheSaved;
void LoadPipelineCache() {
    std::vector<char> blob;
    if (FILE* f = fopen((ExeDir() + "/pipeline_cache.bin").c_str(), "rb")) {
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        if (n > 0) { blob.resize(size_t(n)); if (fread(blob.data(), 1, blob.size(), f) != blob.size()) blob.clear(); }
        fclose(f);
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = blob.size(); ci.pInitialData = blob.empty() ? nullptr : blob.data();
    // The driver validates the header (vendor/device/UUID) and ignores a blob from another GPU or driver.
    if (vkCreatePipelineCache(V->dev, &ci, nullptr, &g_pipeCache) != VK_SUCCESS) {
        ci.initialDataSize = 0; ci.pInitialData = nullptr;
        vkCreatePipelineCache(V->dev, &ci, nullptr, &g_pipeCache);
    }
    g_pipeCacheSaved = std::chrono::steady_clock::now();
    fprintf(stderr, "[render] pipeline cache: %zu bytes loaded\n", blob.size());
}
void SavePipelineCache(bool force) {
    if (!g_pipeCache || !g_pipeCacheDirty) return;
    auto now = std::chrono::steady_clock::now();
    if (!force && now - g_pipeCacheSaved < std::chrono::seconds(5)) return;
    size_t n = 0;
    if (vkGetPipelineCacheData(V->dev, g_pipeCache, &n, nullptr) != VK_SUCCESS || !n) return;
    std::vector<char> blob(n);
    if (vkGetPipelineCacheData(V->dev, g_pipeCache, &n, blob.data()) != VK_SUCCESS) return;
    std::string path = ExeDir() + "/pipeline_cache.bin", tmp = path + ".tmp";
    if (FILE* f = fopen(tmp.c_str(), "wb")) {
        bool ok = fwrite(blob.data(), 1, n, f) == n; fclose(f);
        std::error_code ec;
        if (ok) std::filesystem::rename(tmp, path, ec);
    }
    g_pipeCacheDirty = false; g_pipeCacheSaved = now;
}

bool CompileGlsl(const std::string& src, glslang_stage_t stage, std::vector<uint32_t>& spirv, std::string& log) {
    static bool init = (glslang_initialize_process(), true); (void)init;
    glslang_input_t in{}; in.language = GLSLANG_SOURCE_GLSL; in.stage = stage; in.client = GLSLANG_CLIENT_VULKAN;
    in.client_version = GLSLANG_TARGET_VULKAN_1_1; in.target_language = GLSLANG_TARGET_SPV; in.target_language_version = GLSLANG_TARGET_SPV_1_3;
    in.code = src.c_str(); in.default_version = 450; in.default_profile = GLSLANG_NO_PROFILE; in.messages = GLSLANG_MSG_DEFAULT_BIT;
    in.resource = glslang_default_resource();
    glslang_shader_t* sh = glslang_shader_create(&in);
    bool ok = glslang_shader_preprocess(sh, &in) && glslang_shader_parse(sh, &in);
    if (!ok) { log = glslang_shader_get_info_log(sh); glslang_shader_delete(sh); return false; }
    glslang_program_t* prog = glslang_program_create(); glslang_program_add_shader(prog, sh);
    ok = glslang_program_link(prog, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT);
    if (ok) {
        glslang_program_SPIRV_generate(prog, stage);
        spirv.resize(glslang_program_SPIRV_get_size(prog));
        glslang_program_SPIRV_get(prog, spirv.data());
    } else log = glslang_program_get_info_log(prog);
    glslang_program_delete(prog); glslang_shader_delete(sh);
    return ok;
}
VkShaderModule MakeModule(const std::vector<uint32_t>& spv) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; ci.codeSize = spv.size() * 4; ci.pCode = spv.data();
    VkShaderModule m = VK_NULL_HANDLE; vkCreateShaderModule(V->dev, &ci, nullptr, &m); return m;
}
VkShaderModule CompileModule(const std::string& src, glslang_stage_t st, const char* what) {
    uint64_t key = HashText(src, uint32_t(st) * 0x9E3779B9u + 1u);
    {
        std::lock_guard<std::mutex> l(g_spvLock);
        auto it = g_spvCache.find(key);
        if (it != g_spvCache.end()) { VkShaderModule m = MakeModule(it->second); if (m) return m; }
    }
    std::vector<uint32_t> spv; std::string log;
    if (!CompileGlsl(src, st, spv, log)) { fprintf(stderr, "[render] %s failed to compile:\n%s\n", what, log.c_str()); return VK_NULL_HANDLE; }
    StoreShader(key, spv);
    return MakeModule(spv);
}

Shader* GetShader(uint64_t hash, bool pixel) {
    auto it = g_shaders.find(hash);
    if (it != g_shaders.end()) return it->second.get();
    auto sh = std::make_unique<Shader>();
    auto mc = g_microcode.find(hash);
    if (mc == g_microcode.end()) { sh->failed = true; }
    else {
        sh->t = xenos::Translate(pixel ? xenos::ShaderStage::kPixel : xenos::ShaderStage::kVertex, mc->second.data(), mc->second.size());
        if (!sh->t.ok) { fprintf(stderr, "[render] shader %016llx: %s\n", (unsigned long long)hash, sh->t.error.c_str()); sh->failed = true; }
        else {
            char name[64]; snprintf(name, sizeof name, "%s %016llx", pixel ? "pixel shader" : "vertex shader", (unsigned long long)hash);
            sh->mod = CompileModule(sh->t.glsl, pixel ? GLSLANG_STAGE_FRAGMENT : GLSLANG_STAGE_VERTEX, name);
            if (!sh->mod) sh->failed = true;
        }
    }
    Shader* p = sh.get(); g_shaders[hash] = std::move(sh);
    return p;
}

// Rect lists: a geometry shader turns each 3-vertex rectangle into a quad (4th corner = a + b - c).
std::string RectGsSource(uint32_t mask) {
    std::string d, copy, extra;
    for (int i = 0; i < 16; i++) if (mask & (1u << i)) {
        d += "layout(location = " + std::to_string(i) + ") in vec4 i" + std::to_string(i) + "[];\n";
        d += "layout(location = " + std::to_string(i) + ") out vec4 o" + std::to_string(i) + ";\n";
        copy += " o" + std::to_string(i) + " = i" + std::to_string(i) + "[v];";
        extra += " o" + std::to_string(i) + " = i" + std::to_string(i) + "[a] + i" + std::to_string(i) + "[b] - i" + std::to_string(i) + "[c];";
    }
    return "#version 450\nlayout(triangles) in;\nlayout(triangle_strip, max_vertices = 4) out;\n" + d +
           "void emitv(int v) {" + copy + " gl_Position = gl_in[v].gl_Position; gl_PointSize = 1.0; EmitVertex(); }\n"
           "void main() {\n"
           "    vec2 p0 = gl_in[0].gl_Position.xy / gl_in[0].gl_Position.w, p1 = gl_in[1].gl_Position.xy / gl_in[1].gl_Position.w, p2 = gl_in[2].gl_Position.xy / gl_in[2].gl_Position.w;\n"
           "    float d01 = dot(p0 - p1, p0 - p1), d12 = dot(p1 - p2, p1 - p2), d20 = dot(p2 - p0, p2 - p0);\n"
           "    int c = (d01 >= d12 && d01 >= d20) ? 2 : (d12 >= d20 ? 0 : 1);\n"
           "    int a = (c + 1) % 3, b = (c + 2) % 3;\n"
           "    emitv(c); emitv(a); emitv(b);\n"
           "   " + extra + "\n"
           "    gl_Position = gl_in[a].gl_Position + gl_in[b].gl_Position - gl_in[c].gl_Position; gl_PointSize = 1.0;\n"
           "    EmitVertex(); EndPrimitive();\n}\n";
}
std::unordered_map<uint32_t, VkShaderModule> g_rectGsByMask;
VkShaderModule RectGs(uint32_t mask);
const char* kBlitVs = R"(#version 450
layout(location = 0) out vec2 uv;
void main() { vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2); uv = p; gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }
)";
const char* kBlitPs = R"(#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform P { vec4 rect; } pc;   // xy offset, zw size in normalized source coordinates
void main() { o = vec4(texture(src, pc.rect.xy + uv * pc.rect.zw).rgb, 1.0); }
)";

const char* kScalePs = R"(#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform P { vec4 rect; vec4 scale; } pc;
void main() { o = texture(src, pc.rect.xy + uv * pc.rect.zw) * pc.scale.x; }
)";
VkShaderModule g_scalePs = VK_NULL_HANDLE; VkPipelineLayout g_scaleLayout = VK_NULL_HANDLE;
std::unordered_map<uint32_t, VkPipeline> g_scalePipes;
VkPipeline ScalePipeline(VkFormat fmt);

struct PipeKey {
    uint64_t vs, ps; uint32_t prim, colorFmts, depthFmt, colorMask, blend[4], depthCtl, stencilBf, mode, restart, numColor;
    bool operator==(const PipeKey& o) const { return !memcmp(this, &o, sizeof *this); }
};
struct PipeKeyHash { size_t operator()(const PipeKey& k) const { return (size_t)Fnv64(&k, sizeof k); } };
std::unordered_map<PipeKey, VkPipeline, PipeKeyHash> g_pipelines;

VkFormat ColorFormat(uint32_t f) {
    switch (f) {
    case 0: case 1: return VK_FORMAT_R8G8B8A8_UNORM;
    case 2: case 10: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 3: case 12: case 7: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 4: return VK_FORMAT_R16G16_SNORM;
    case 5: return VK_FORMAT_R16G16B16A16_SNORM;
    case 6: return VK_FORMAT_R16G16_SFLOAT;
    case 14: return VK_FORMAT_R32_SFLOAT;
    case 15: return VK_FORMAT_R32G32_SFLOAT;
    default: return VK_FORMAT_R8G8B8A8_UNORM;
    }
}
VkBlendFactor BlendFactor(uint32_t f) {
    switch (f) {
    case 0: return VK_BLEND_FACTOR_ZERO; case 1: return VK_BLEND_FACTOR_ONE;
    case 4: return VK_BLEND_FACTOR_SRC_COLOR; case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6: return VK_BLEND_FACTOR_SRC_ALPHA; case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_COLOR; case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_DST_ALPHA; case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR; case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
VkBlendOp BlendOp(uint32_t f) {
    switch (f) { case 1: return VK_BLEND_OP_SUBTRACT; case 2: return VK_BLEND_OP_MIN; case 3: return VK_BLEND_OP_MAX; case 4: return VK_BLEND_OP_REVERSE_SUBTRACT; default: return VK_BLEND_OP_ADD; }
}

VkPipeline GetPipeline(const PipeKey& k, Shader* vs, Shader* ps, const VkFormat* colorFormats, bool depth) {
    auto it = g_pipelines.find(k);
    if (it != g_pipelines.end()) return it->second;
    VkPipelineShaderStageCreateInfo st[3]{};
    uint32_t ns = 0;
    st[ns] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs->mod, "main", nullptr}; ns++;
    if (k.prim == 8) {
        VkShaderModule gs = RectGs(vs->t.interpolatorsWritten);
        if (!gs) { g_pipelines[k] = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
        st[ns] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_GEOMETRY_BIT, gs, "main", nullptr}; ns++;
    }
    st[ns] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ps->mod, "main", nullptr}; ns++;
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    switch (k.prim) {
    case 1: ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
    case 2: ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case 3: ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case 5: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
    case 6: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    default: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    }
    ia.primitiveRestartEnable = k.restart && (k.prim == 3 || k.prim == 5 || k.prim == 6);
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.0f;
    uint32_t mode = k.mode;
    bool isTri = k.prim >= 4;
    rs.cullMode = isTri ? ((mode & 1 ? VK_CULL_MODE_FRONT_BIT : 0) | (mode & 2 ? VK_CULL_MODE_BACK_BIT : 0)) : VK_CULL_MODE_NONE;
    if (k.prim == 8 || k.prim == 13) rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = (mode & 4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.depthClampEnable = VK_FALSE;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    uint32_t dc = k.depthCtl;
    if (depth) {
        ds.depthTestEnable = (dc >> 1) & 1; ds.depthWriteEnable = ((dc >> 1) & 1) && ((dc >> 2) & 1);
        ds.depthCompareOp = VkCompareOp((dc >> 4) & 7);
        ds.stencilTestEnable = dc & 1;
        ds.front.compareOp = VkCompareOp((dc >> 8) & 7); ds.front.failOp = VkStencilOp((dc >> 11) & 7);
        ds.front.passOp = VkStencilOp((dc >> 14) & 7); ds.front.depthFailOp = VkStencilOp((dc >> 17) & 7);
        if ((dc >> 7) & 1) {
            ds.back.compareOp = VkCompareOp((dc >> 20) & 7); ds.back.failOp = VkStencilOp((dc >> 23) & 7);
            ds.back.passOp = VkStencilOp((dc >> 26) & 7); ds.back.depthFailOp = VkStencilOp((dc >> 29) & 7);
        } else ds.back = ds.front;
    }
    VkPipelineColorBlendAttachmentState ba[4]{};
    for (uint32_t i = 0; i < k.numColor; i++) {
        uint32_t b = k.blend[i];
        ba[i].colorWriteMask = (k.colorMask >> (4 * i)) & 0xF;
        ba[i].srcColorBlendFactor = BlendFactor(b & 0x1F); ba[i].colorBlendOp = BlendOp((b >> 5) & 7); ba[i].dstColorBlendFactor = BlendFactor((b >> 8) & 0x1F);
        ba[i].srcAlphaBlendFactor = BlendFactor((b >> 16) & 0x1F); ba[i].alphaBlendOp = BlendOp((b >> 21) & 7); ba[i].dstAlphaBlendFactor = BlendFactor((b >> 24) & 0x1F);
        bool noBlend = ba[i].srcColorBlendFactor == VK_BLEND_FACTOR_ONE && ba[i].dstColorBlendFactor == VK_BLEND_FACTOR_ZERO && ba[i].colorBlendOp == VK_BLEND_OP_ADD &&
                       ba[i].srcAlphaBlendFactor == VK_BLEND_FACTOR_ONE && ba[i].dstAlphaBlendFactor == VK_BLEND_FACTOR_ZERO && ba[i].alphaBlendOp == VK_BLEND_OP_ADD;
        ba[i].blendEnable = !noBlend && colorFormats[i] != VK_FORMAT_R32_SFLOAT && colorFormats[i] != VK_FORMAT_R32G32_SFLOAT;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount = k.numColor; cb.pAttachments = ba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                            VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dy.dynamicStateCount = 6; dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = k.numColor; ri.pColorAttachmentFormats = colorFormats;
    ri.depthAttachmentFormat = depth ? g_depthFormat : VK_FORMAT_UNDEFINED; ri.stencilAttachmentFormat = ri.depthAttachmentFormat;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; gi.pNext = &ri;
    gi.stageCount = ns; gi.pStages = st; gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp;
    gi.pRasterizationState = &rs; gi.pMultisampleState = &ms; gi.pDepthStencilState = &ds; gi.pColorBlendState = &cb; gi.pDynamicState = &dy;
    gi.layout = g_pipeLayout;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(V->dev, g_pipeCache, 1, &gi, nullptr, &p) != VK_SUCCESS) fprintf(stderr, "[render] pipeline creation failed\n");
    else g_pipeCacheDirty = true;
    g_pipelines[k] = p;
    return p;
}

// ---------------------------------------------------------------- render targets (EDRAM)
struct RenderTarget { Image im; uint32_t base, pitch, format, guestH; bool depth; bool cleared = false; VkImageView attach = VK_NULL_HANDLE; };
std::unordered_map<uint64_t, std::unique_ptr<RenderTarget>> g_rts;
uint64_t RtKey(uint32_t base, uint32_t pitch, uint32_t fmt, bool depth) { return uint64_t(base) | (uint64_t(pitch) << 12) | (uint64_t(fmt) << 28) | (uint64_t(depth) << 34); }

// ---------------------------------------------------------------- resolved images and textures
struct Resolved { Image im; uint32_t base, pitch, height, format; uint64_t frame; bool swapRB = false; };
std::map<uint32_t, std::unique_ptr<Resolved>> g_resolved;    // by destination physical address
struct Texture { Image im; uint64_t hash = 0, checkedFrame = ~0ull; bool valid = false; };
std::unordered_map<uint64_t, std::unique_ptr<Texture>> g_textures;   // by fetch-constant identity
std::unordered_map<uint64_t, VkSampler> g_samplers;
Image g_dummy2D, g_dummyCube, g_dummy3D, g_dummyArray;

// ---------------------------------------------------------------- command recording
VkCommandBuffer Cmd();
bool g_inPass = false;
struct PassState { RenderTarget* color[4]{}; uint32_t numColor = 0; RenderTarget* depth = nullptr; } g_pass;

void FullBarrier(VkCommandBuffer c) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}
void ToGeneral(VkCommandBuffer c, Image& im) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = im.img;
    b.subresourceRange = {im.depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT), 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}
void EndPass() {
    if (!g_inPass) return;
    vkCmdEndRendering(Cmd());
    FullBarrier(Cmd());
    g_inPass = false;
}

void SubmitFrame(bool wait) {
    Frame& f = g_frames[g_cur];
    if (!f.recording) return;
    EndPass();
    vkEndCommandBuffer(f.cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &f.cmd;
    { std::lock_guard<std::mutex> l(*V->queueLock); vkQueueSubmit(V->queue, 1, &si, f.fence); }
    f.recording = false;
    if (wait) vkWaitForFences(V->dev, 1, &f.fence, VK_TRUE, UINT64_MAX);
}
void BeginFrame() {
    Frame& f = g_frames[g_cur];
    if (f.recording) return;
    vkWaitForFences(V->dev, 1, &f.fence, VK_TRUE, UINT64_MAX);
    vkResetFences(V->dev, 1, &f.fence);
    for (Image* im : g_deferredDestroy[g_cur]) { im->Destroy(); delete im; }
    g_deferredDestroy[g_cur].clear();
    vkResetCommandPool(V->dev, f.pool, 0);
    vkResetDescriptorPool(V->dev, f.dpool, 0);
    f.ring.used = 0; f.vbCache.clear();
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &bi);
    f.recording = true;
}
VkCommandBuffer Cmd() { BeginFrame(); return g_frames[g_cur].cmd; }

// Ring allocation; on overflow the frame is submitted and waited for, then recording continues.
size_t RingAlloc(size_t size, size_t align) {
    Frame& f = g_frames[g_cur];
    size_t off = (f.ring.used + align - 1) & ~(align - 1);
    if (off + size > f.ring.size) {
        // keep pass state; restart in a fresh command buffer of the same frame slot
        bool wasInPass = g_inPass;
        SubmitFrame(true);
        (void)wasInPass;
        BeginFrame();
        off = 0;
    }
    f.ring.used = off + size;
    return off;
}

// ---------------------------------------------------------------- setup
bool Init() {
    g_tried = true;
    V = PresenterWaitVulkan();
    if (!V) return false;
    if (!V->dynamicRendering) { fprintf(stderr, "[render] this GPU/driver lacks dynamic rendering (Vulkan 1.3) - only the front buffer is shown\n"); return false; }
    S = std::clamp<uint32_t>(g_settings.resolutionScale, 1, 3);
    LoadShaderCache();
    LoadPipelineCache();
    vkGetPhysicalDeviceProperties(V->phys, &g_props);
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(V->phys, VK_FORMAT_D32_SFLOAT_S8_UINT, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) g_depthFormat = VK_FORMAT_D24_UNORM_S8_UINT;

    // descriptor set layout
    std::vector<VkDescriptorSetLayoutBinding> b;
    b.push_back({0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_GEOMETRY_BIT, nullptr});
    b.push_back({1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr});
    for (uint32_t i = 0; i < kPsTexCount; i++) b.push_back({kPsTexBase + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    for (uint32_t i = 0; i < kVsTexCount; i++) b.push_back({kVsTexBase + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr});
    for (uint32_t i = 0; i < kPsTexCount; i++) b.push_back({24 + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    for (uint32_t i = 0; i < kVsTexCount; i++) b.push_back({48 + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr});
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; lci.bindingCount = (uint32_t)b.size(); lci.pBindings = b.data();
    vkCreateDescriptorSetLayout(V->dev, &lci, nullptr, &g_setLayout);
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &g_setLayout;
    vkCreatePipelineLayout(V->dev, &pli, nullptr, &g_pipeLayout);
    // blit pipeline (front buffer -> presentation image, applies the fetch swizzle via the image view)
    VkDescriptorSetLayoutBinding bb{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    lci.bindingCount = 1; lci.pBindings = &bb; vkCreateDescriptorSetLayout(V->dev, &lci, nullptr, &g_blitSetLayout);
    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    pli.pSetLayouts = &g_blitSetLayout; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(V->dev, &pli, nullptr, &g_blitLayout);
    g_blitVs = CompileModule(kBlitVs, GLSLANG_STAGE_VERTEX, "blit vs");
    g_blitPs = CompileModule(kBlitPs, GLSLANG_STAGE_FRAGMENT, "blit ps");
    if (V->geometryShader) g_rectGs = RectGs(1);
    {
        VkPipelineShaderStageCreateInfo st[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, g_blitVs, "main", nullptr},
                                              {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, g_blitPs, "main", nullptr}};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = 1; vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState ba{}; ba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount = 1; cb.pAttachments = &ba;
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
        VkFormat f = VK_FORMAT_R8G8B8A8_UNORM;
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &f;
        VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; gi.pNext = &ri; gi.stageCount = 2; gi.pStages = st;
        gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp; gi.pRasterizationState = &rs; gi.pMultisampleState = &ms;
        gi.pColorBlendState = &cb; gi.pDynamicState = &dy; gi.layout = g_blitLayout;
        if (vkCreateGraphicsPipelines(V->dev, g_pipeCache, 1, &gi, nullptr, &g_blitPipe) != VK_SUCCESS) return false;
    }
    {
        VkPushConstantRange r2{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo l2{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; l2.setLayoutCount = 1; l2.pSetLayouts = &g_blitSetLayout;
        l2.pushConstantRangeCount = 1; l2.pPushConstantRanges = &r2;
        vkCreatePipelineLayout(V->dev, &l2, nullptr, &g_scaleLayout);
        g_scalePs = CompileModule(kScalePs, GLSLANG_STAGE_FRAGMENT, "scale ps");
    }
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; sci.maxLod = 0;
    vkCreateSampler(V->dev, &sci, nullptr, &g_linearClamp);

    for (Frame& f : g_frames) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.queueFamilyIndex = V->qfam;
        vkCreateCommandPool(V->dev, &pci, nullptr, &f.pool);
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool = f.pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        vkAllocateCommandBuffers(V->dev, &cai, &f.cmd);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fci.flags = VK_FENCE_CREATE_SIGNALED_BIT; vkCreateFence(V->dev, &fci, nullptr, &f.fence);
        if (!CreateRing(f.ring, kRingSize)) { fprintf(stderr, "[render] could not allocate the upload buffer\n"); return false; }
        VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 8192}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8192}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 65536}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 8192; dpi.poolSizeCount = 3; dpi.pPoolSizes = ps;
        vkCreateDescriptorPool(V->dev, &dpi, nullptr, &f.dpool);
    }
    // dummy textures (opaque black) for unsupported formats
    CreateImage(g_dummy2D, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, false, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    CreateImage(g_dummyCube, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, false, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 6, true);
    VkCommandBuffer c = Cmd();
    CreateImage(g_dummyArray, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, false, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1, false, true);
    ToGeneral(c, g_dummy2D); ToGeneral(c, g_dummyCube); ToGeneral(c, g_dummyArray);
    { VkClearColorValue blk{{0, 0, 0, 1}}; VkImageSubresourceRange r1{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}; vkCmdClearColorImage(c, g_dummyArray.img, VK_IMAGE_LAYOUT_GENERAL, &blk, 1, &r1); }
    if (CreateImage3D(g_dummy3D, VK_FORMAT_R8G8B8A8_UNORM, 2, 2, 2)) {   // identity colour lookup, so a missing LUT keeps colours
        ToGeneral(c, g_dummy3D);
        size_t off = RingAlloc(32, 16); uint8_t* p = g_frames[g_cur].ring.ptr + off;
        for (int z = 0; z < 2; z++) for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++) { uint8_t* t = p + ((z * 2 + y) * 2 + x) * 4; t[0] = x * 255; t[1] = y * 255; t[2] = z * 255; t[3] = 255; }
        VkBufferImageCopy bc{}; bc.bufferOffset = off; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bc.imageExtent = {2, 2, 2};
        vkCmdCopyBufferToImage(c, g_frames[g_cur].ring.buf, g_dummy3D.img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
    }
    VkClearColorValue black{{0, 0, 0, 1}}; VkImageSubresourceRange rr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(c, g_dummy2D.img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &rr);
    rr.layerCount = 6; vkCmdClearColorImage(c, g_dummyCube.img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &rr);
    FullBarrier(c);
    fprintf(stderr, "[render] draw renderer ready (internal scale %ux, depth format %s, rect lists %s)\n", S,
            g_depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT ? "D32S8" : "D24S8", g_rectGs ? "geometry shader" : "unsupported");
    return true;
}

VkShaderModule RectGs(uint32_t mask) {
    auto it = g_rectGsByMask.find(mask);
    if (it != g_rectGsByMask.end()) return it->second;
    VkShaderModule m = V->geometryShader ? CompileModule(RectGsSource(mask), GLSLANG_STAGE_GEOMETRY, "rect gs") : VK_NULL_HANDLE;
    g_rectGsByMask[mask] = m;
    return m;
}

VkPipeline ScalePipeline(VkFormat fmt) {
    auto it = g_scalePipes.find((uint32_t)fmt);
    if (it != g_scalePipes.end()) return it->second;
    VkPipelineShaderStageCreateInfo st[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, g_blitVs, "main", nullptr},
                                          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, g_scalePs, "main", nullptr}};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba{}; ba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &fmt;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; gi.pNext = &ri; gi.stageCount = 2; gi.pStages = st;
    gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp; gi.pRasterizationState = &rs; gi.pMultisampleState = &ms;
    gi.pColorBlendState = &cb; gi.pDynamicState = &dy; gi.layout = g_scaleLayout;
    VkPipeline pp = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(V->dev, g_pipeCache, 1, &gi, nullptr, &pp) != VK_SUCCESS) pp = VK_NULL_HANDLE;
    g_scalePipes[(uint32_t)fmt] = pp;
    return pp;
}
void EndPass();
// Copies a region of `src` into `dst` (in pixels of each image) multiplying by `scale`; linear filtering averages MSAA samples.
bool ScaledCopy(Image& src, VkRect2D sr, Image& dst, VkRect2D dr, float scale) {
    VkPipeline pp = ScalePipeline(dst.fmt);
    if (!pp) return false;
    EndPass();
    VkCommandBuffer c = Cmd();
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = g_frames[g_cur].dpool; dai.descriptorSetCount = 1; dai.pSetLayouts = &g_blitSetLayout;
    VkDescriptorSet set; if (vkAllocateDescriptorSets(V->dev, &dai, &set) != VK_SUCCESS) return false;
    VkDescriptorImageInfo ii{g_linearClamp, src.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wd{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii, nullptr, nullptr};
    vkUpdateDescriptorSets(V->dev, 1, &wd, 0, nullptr);
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; ca.imageView = dst.view; ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO}; ri.renderArea = dr; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    vkCmdBeginRendering(c, &ri);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pp);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, g_scaleLayout, 0, 1, &set, 0, nullptr);
    VkViewport vp{float(dr.offset.x), float(dr.offset.y), float(dr.extent.width), float(dr.extent.height), 0, 1};
    vkCmdSetViewport(c, 0, 1, &vp); vkCmdSetScissor(c, 0, 1, &dr);
    float pcv[8] = {float(sr.offset.x) / src.w, float(sr.offset.y) / src.h, float(sr.extent.width) / src.w, float(sr.extent.height) / src.h, scale, 0, 0, 0};
    vkCmdPushConstants(c, g_scaleLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pcv);
    vkCmdDraw(c, 3, 1, 0, 0);
    vkCmdEndRendering(c);
    FullBarrier(c);
    return true;
}

// ---------------------------------------------------------------- render targets
RenderTarget* GetRT(uint32_t base, uint32_t pitch, uint32_t fmt, bool depth, uint32_t neededH) {
    if (!pitch) return nullptr;
    neededH = std::clamp<uint32_t>((neededH + 15) & ~15u, 16, 2048);
    uint64_t key = RtKey(base, pitch, depth ? 0 : fmt, depth);
    auto it = g_rts.find(key);
    if (it != g_rts.end() && it->second->guestH >= neededH) return it->second.get();
    if (it != g_rts.end()) {   // grow: replace (contents are lost)
        EndPass();
        Image* old = new Image(it->second->im); g_deferredDestroy[g_cur].push_back(old);
        g_rts.erase(it);
    }
    auto rt = std::make_unique<RenderTarget>();
    rt->base = base; rt->pitch = pitch; rt->format = fmt; rt->depth = depth; rt->guestH = std::max<uint32_t>(neededH, 720);
    VkFormat vf = depth ? g_depthFormat : ColorFormat(fmt);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                              (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (!CreateImage(rt->im, vf, pitch * S, rt->guestH * S, depth, usage)) { fprintf(stderr, "[render] render target allocation failed\n"); return nullptr; }
    if (!depth && fmt == 1) rt->attach = SwizzledView(rt->im, 0x688, true);   // 8_8_8_8_GAMMA: encode on write (sRGB approximates the Xenos curve)
    EndPass();
    VkCommandBuffer c = Cmd();
    ToGeneral(c, rt->im);
    if (depth) {
        VkClearDepthStencilValue z{1.0f, 0}; VkImageSubresourceRange r{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(c, rt->im.img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &r);
    } else {
        VkClearColorValue z{}; VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(c, rt->im.img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &r);
    }
    FullBarrier(c);
    if (g_log) fprintf(stderr, "[render] new %s target base %u pitch %u format %u -> %ux%u\n", depth ? "depth" : "color", base, pitch, fmt, rt->im.w, rt->im.h);
    RenderTarget* p = rt.get(); g_rts[key] = std::move(rt);
    return p;
}

// ---------------------------------------------------------------- textures
struct TexFormatInfo { VkFormat vk; uint32_t blockW, bytesPerBlock; int convert; };   // convert: 0 copy, 1 4444->8888, 2 1555->8888, 3 24_8->R32F
bool TexFormat(uint32_t f, TexFormatInfo& o) {
    switch (f) {
    case 2: o = {VK_FORMAT_R8_UNORM, 1, 1, 0}; return true;
    case 3: o = {VK_FORMAT_R8G8B8A8_UNORM, 1, 2, 2}; return true;
    case 4: o = {VK_FORMAT_B5G6R5_UNORM_PACK16, 1, 2, 0}; return true;
    case 6: o = {VK_FORMAT_R8G8B8A8_UNORM, 1, 4, 0}; return true;
    case 7: case 54: o = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 1, 4, 0}; return true;
    case 10: o = {VK_FORMAT_R8G8_UNORM, 1, 2, 0}; return true;
    case 15: o = {VK_FORMAT_R8G8B8A8_UNORM, 1, 2, 1}; return true;
    case 18: o = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 8, 0}; return V->bc;
    case 19: o = {VK_FORMAT_BC2_UNORM_BLOCK, 4, 16, 0}; return V->bc;
    case 20: o = {VK_FORMAT_BC3_UNORM_BLOCK, 4, 16, 0}; return V->bc;
    case 22: case 23: o = {VK_FORMAT_R32_SFLOAT, 1, 4, 3}; return true;
    case 24: o = {VK_FORMAT_R16_UNORM, 1, 2, 0}; return true;
    case 25: o = {VK_FORMAT_R16G16_UNORM, 1, 4, 0}; return true;
    case 26: o = {VK_FORMAT_R16G16B16A16_UNORM, 1, 8, 0}; return true;
    case 27: case 30: o = {VK_FORMAT_R16_SFLOAT, 1, 2, 0}; return true;
    case 28: case 31: o = {VK_FORMAT_R16G16_SFLOAT, 1, 4, 0}; return true;
    case 29: case 32: o = {VK_FORMAT_R16G16B16A16_SFLOAT, 1, 8, 0}; return true;
    case 36: o = {VK_FORMAT_R32_SFLOAT, 1, 4, 0}; return true;
    case 37: o = {VK_FORMAT_R32G32_SFLOAT, 1, 8, 0}; return true;
    case 38: o = {VK_FORMAT_R32G32B32A32_SFLOAT, 1, 16, 0}; return true;
    case 49: o = {VK_FORMAT_BC5_UNORM_BLOCK, 4, 16, 0}; return V->bc;
    case 59: o = {VK_FORMAT_BC4_UNORM_BLOCK, 4, 8, 0}; return V->bc;
    default: return false;
    }
}
uint32_t Log2(uint32_t v) { uint32_t r = 0; while ((1u << (r + 1)) <= v) r++; return r; }

// ---- capture: write decoded textures (mip 0, first slice) as PPM + alpha PGM
void Bc1Colors(const uint8_t* b, uint8_t out[4][4], bool bc1) {
    uint16_t c0 = uint16_t(b[0] | (b[1] << 8)), c1 = uint16_t(b[2] | (b[3] << 8));
    auto ex = [](uint16_t c, uint8_t* o) { o[0] = uint8_t(((c >> 11) & 31) * 255 / 31); o[1] = uint8_t(((c >> 5) & 63) * 255 / 63); o[2] = uint8_t((c & 31) * 255 / 31); o[3] = 255; };
    ex(c0, out[0]); ex(c1, out[1]);
    if (c0 > c1 || !bc1) for (int k = 0; k < 4; k++) { out[2][k] = uint8_t((2 * out[0][k] + out[1][k]) / 3); out[3][k] = uint8_t((out[0][k] + 2 * out[1][k]) / 3); }
    else { for (int k = 0; k < 4; k++) { out[2][k] = uint8_t((out[0][k] + out[1][k]) / 2); out[3][k] = 0; } out[3][3] = 0; }
}
void DumpTexture(const std::string& path, const uint8_t* data, uint32_t bw, uint32_t bh, uint32_t w, uint32_t h, VkFormat vk, uint32_t bpb) {
    std::vector<uint8_t> rgba((size_t)w * h * 4, 0);
    for (uint32_t by = 0; by < bh; by++) for (uint32_t bx = 0; bx < bw; bx++) {
        const uint8_t* b = data + ((size_t)by * bw + bx) * bpb;
        auto put = [&](uint32_t x, uint32_t y, const uint8_t* px) { if (x < w && y < h) memcpy(&rgba[((size_t)y * w + x) * 4], px, 4); };
        switch (vk) {
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC3_UNORM_BLOCK: {
            const uint8_t* cb = vk == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? b : b + 8;
            uint8_t pal[4][4]; Bc1Colors(cb, pal, vk == VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
            uint32_t idx = cb[4] | (cb[5] << 8) | (cb[6] << 16) | (uint32_t(cb[7]) << 24);
            uint8_t a[16]; for (int i = 0; i < 16; i++) a[i] = 255;
            if (vk == VK_FORMAT_BC2_UNORM_BLOCK) for (int i = 0; i < 16; i++) a[i] = uint8_t(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
            if (vk == VK_FORMAT_BC3_UNORM_BLOCK) {
                uint8_t a0 = b[0], a1 = b[1]; uint64_t bits = 0; for (int i = 0; i < 6; i++) bits |= uint64_t(b[2 + i]) << (8 * i);
                for (int i = 0; i < 16; i++) { uint32_t ai = (bits >> (3 * i)) & 7;
                    a[i] = ai == 0 ? a0 : ai == 1 ? a1 : a0 > a1 ? uint8_t(((8 - ai) * a0 + (ai - 1) * a1) / 7) : ai == 6 ? 0 : ai == 7 ? 255 : uint8_t(((6 - ai) * a0 + (ai - 1) * a1) / 5); }
            }
            for (int i = 0; i < 16; i++) { uint8_t px[4]; memcpy(px, pal[(idx >> (2 * i)) & 3], 4); if (vk != VK_FORMAT_BC1_RGBA_UNORM_BLOCK) px[3] = a[i]; put(bx * 4 + (i & 3), by * 4 + (i >> 2), px); }
            break; }
        case VK_FORMAT_R8G8B8A8_UNORM: put(bx, by, b); break;
        case VK_FORMAT_R8_UNORM: { uint8_t px[4] = {b[0], b[0], b[0], 255}; put(bx, by, px); break; }
        case VK_FORMAT_R8G8_UNORM: { uint8_t px[4] = {b[0], b[1], 0, 255}; put(bx, by, px); break; }
        default: { uint8_t px[4] = {b[0], bpb > 1 ? b[1] : b[0], bpb > 2 ? b[2] : b[0], 255}; put(bx, by, px); break; }
        }
    }
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (size_t i = 0; i < (size_t)w * h; i++) fwrite(&rgba[i * 4], 1, 3, f);
    fclose(f);
    std::string ap = path.substr(0, path.size() - 4) + "_alpha.pgm";
    if ((f = fopen(ap.c_str(), "wb"))) { fprintf(f, "P5\n%u %u\n255\n", w, h); for (size_t i = 0; i < (size_t)w * h; i++) fputc(rgba[i * 4 + 3], f); fclose(f); }
}
std::set<uint64_t> g_dumpedTextures;

// Returns an image view for texture fetch constant `slot`, as the kind of sampler the shader declared
// (`dim`: 0/1 = 2D, 2 = 3D, 3 = cube), uploading mip 0 from guest memory if needed. Handles every combination of
// what the data is (1D/2D, stacked 2D array, volume, cube) and what the shader samples it as.
VkImageView TextureView(uint32_t slot, uint32_t dim, bool& isCube, VkImageView* arrayView = nullptr) {
    uint32_t f[6]; for (int i = 0; i < 6; i++) f[i] = Reg(R_CONST_FETCH + slot * 6 + i);
    isCube = dim == 3;
    if (arrayView) *arrayView = SwizzledView(g_dummyArray, 0x688);
    VkImageView fallback = dim == 3 ? SwizzledView(g_dummyCube, 0x688) : dim == 2 ? SwizzledView(g_dummy3D, 0x688) : g_dummy2D.view;
    if ((f[0] & 3) != 2) return fallback;
    uint32_t swz = (f[3] >> 1) & 0xFFF;
    uint32_t fmt = f[1] & 0x3F, endian = (f[1] >> 6) & 3, base = f[1] & 0xFFFFF000;
    bool stacked = (f[1] >> 10) & 1, tiled = f[0] >> 31;
    bool gamma = ((f[0] >> 2) & 3) == 3 || ((f[0] >> 4) & 3) == 3 || ((f[0] >> 6) & 3) == 3;
    uint32_t texDim = (f[5] >> 9) & 3;
    uint32_t w, h, d = 1;
    bool volumeData = false;
    // dimension: 0 1D, 1 2D or stacked (stacked flag + stack_depth in size bits 26-31), 2 3D, 3 cube
    if (texDim == 0) { w = (f[2] & 0xFFFFFF) + 1; h = 1; }
    else if (texDim == 1) { w = (f[2] & 0x1FFF) + 1; h = ((f[2] >> 13) & 0x1FFF) + 1; if (stacked) d = ((f[2] >> 26) & 0x3F) + 1; }
    else if (texDim == 2) { w = (f[2] & 0x7FF) + 1; h = ((f[2] >> 11) & 0x7FF) + 1; d = ((f[2] >> 22) & 0x3FF) + 1; volumeData = true; }
    else { w = (f[2] & 0x1FFF) + 1; h = ((f[2] >> 13) & 0x1FFF) + 1; d = 6; }
    // GPU-resolved images (render to texture) are 2D
    if (dim <= 1) {
        auto rit = g_resolved.find(base);
        if (rit != g_resolved.end()) return SwizzledView(rit->second->im, swz, gamma, rit->second->swapRB);
    }
    // 3D lookups of stacked or plain 2D data use a 2D array (layer = z * depth, as Xenia); the 3D binding gets a dummy
    bool asArray = dim == 2 && !volumeData;
    if (asArray && !arrayView) return fallback;
    // how many slices the target image gets
    uint32_t outD;
    if (dim == 3) { if (texDim != 3) return fallback; outD = 6; }
    else if (dim == 2) outD = d;
    else outD = 1;
    TexFormatInfo fi;
    if (!TexFormat(fmt, fi)) {
        static uint64_t warned = 0; if (!(warned & (1ull << fmt))) { warned |= 1ull << fmt; fprintf(stderr, "[render] texture format %u not supported yet\n", fmt); }
        return fallback;
    }
    uint32_t bw = (w + fi.blockW - 1) / fi.blockW, bh = (h + fi.blockW - 1) / fi.blockW;
    uint32_t pitchBlocks = std::max((((f[0] >> 22) & 0x1FF) << 5) / fi.blockW, bw);
    uint32_t pa = (pitchBlocks + 31) & ~31u, ha = (bh + 31) & ~31u;
    uint32_t bppLog2 = Log2(fi.bytesPerBlock);
    size_t rowBytes = ((size_t)pitchBlocks * fi.bytesPerBlock + 255) & ~size_t(255);
    size_t sliceStride = tiled ? (((size_t)pa * ha * fi.bytesPerBlock + 4095) & ~size_t(4095)) : ((rowBytes * ha + 4095) & ~size_t(4095));
    size_t srcSize = volumeData && tiled ? (size_t)pa * ha * ((outD + 3) & ~3u) * fi.bytesPerBlock : sliceStride * outD;
    uint32_t guest = GuestFromPhysical(base);
    if ((uint64_t)guest + srcSize > 0x100000000ull || srcSize > (96u << 20)) return fallback;
    const uint8_t* src = g_base + guest;

    // ---- mip chain (2D, stacked and cube; volumes keep one level). Layout after Xenia's texture_util (BSD-3-Clause):
    // levels 1.. live at mip_address, each level's strides come from the power-of-two-rounded base size aligned to
    // 32x32 blocks, and with packed_mips every level whose short side is <= 16 texels shares one tile ("mip tail").
    uint32_t mipPage = f[5] & 0xFFFFF000, packedMips = (f[5] >> 11) & 1, mipMaxField = (f[4] >> 6) & 0xF;
    uint32_t sizeMaxLevel = Log2(std::max(w, h));
    uint32_t levels = 1;
    // Stacked textures (the jersey letter/number glyph sets) keep their full-size level only: their smaller levels are
    // laid out per glyph in a way not decoded correctly yet, which showed the wrong letters on names seen at a distance.
    if (!volumeData && !stacked && mipPage && base && g_settings.mipmaps) levels = std::min(mipMaxField, sizeMaxLevel) + 1;
    auto log2c = [](uint32_t v) { uint32_t r = 0; while ((1u << r) < v) r++; return r; };
    auto npow2 = [&](uint32_t v) { return 1u << log2c(v); };
    uint32_t packedLevel = packedMips ? (log2c(std::min(w, h)) > 4 ? log2c(std::min(w, h)) - 4 : 0) : 99;
    // block offset of a level inside the packed tail
    auto packedOffset = [&](uint32_t mip, uint32_t& xb, uint32_t& yb) {
        xb = yb = 0;
        uint32_t lw = log2c(w), lh = log2c(h), ls = std::min(lw, lh);
        if (ls > 4 + mip) return false;
        uint32_t pbase = ls > 4 ? ls - 4 : 0, pm = mip - pbase;
        if (pm < 3) { if (lw > lh) yb = 16u >> pm; else xb = 16u >> pm; }
        else { if (lw > lh) xb = (1u << (lw - pbase)) >> (pm - 2); else yb = (1u << (lh - pbase)) >> (pm - 2); }
        xb /= fi.blockW; yb /= fi.blockW;
        return true;
    };
    struct LevelSrc { const uint8_t* p; size_t size, sliceStride, rowBytes; uint32_t pitchBlocks, offX, offY; };
    std::vector<LevelSrc> lsrc(levels);
    lsrc[0] = {src, srcSize, sliceStride, rowBytes, pa, 0, 0};
    if (packedLevel == 0) packedOffset(0, lsrc[0].offX, lsrc[0].offY);
    size_t mipTotal = 0;
    const uint8_t* mipBase = nullptr;
    if (levels > 1) {
        uint32_t mguest = GuestFromPhysical(mipPage);
        size_t off = 0;
        std::vector<size_t> stOff(16), stStride(16), stRow(16); std::vector<uint32_t> stPitch(16);
        uint32_t lastStored = std::min(levels - 1, std::max(packedLevel, 1u));
        for (uint32_t l = 1; l <= lastStored; l++) {
            uint32_t rpt = std::max(npow2(w) >> l, 1u), zst = std::max(npow2(h) >> l, 1u);
            uint32_t pb = (((rpt + fi.blockW - 1) / fi.blockW) + 31) & ~31u;
            size_t rb = (size_t)pb * fi.bytesPerBlock; if (!tiled) rb = (rb + 255) & ~size_t(255);
            uint32_t hb = (((zst + fi.blockW - 1) / fi.blockW) + 31) & ~31u;
            size_t ss = (rb * hb + 4095) & ~size_t(4095);
            stOff[l] = off; stStride[l] = ss; stRow[l] = rb; stPitch[l] = pb;
            off += ss * outD;
        }
        mipTotal = off;
        if ((uint64_t)mguest + mipTotal > 0x100000000ull || mipTotal > (64u << 20)) levels = 1;
        else {
            mipBase = g_base + mguest;
            for (uint32_t l = 1; l < levels; l++) {
                uint32_t st = std::min(l, lastStored);
                lsrc[l] = {mipBase + stOff[st], mipTotal - stOff[st], stStride[st], stRow[st], stPitch[st], 0, 0};
                if (l >= packedLevel) packedOffset(l, lsrc[l].offX, lsrc[l].offY);
            }
            lsrc.resize(levels);
        }
    }

    uint64_t key = Fnv64(f, sizeof f) ^ (uint64_t(dim == 3 ? 3 : asArray ? 4 : dim == 2 ? 2 : 1) * 0x9E3779B97F4A7C15ull);
    auto done = [&](Texture& tt) -> VkImageView {
        VkImageView v = SwizzledView(tt.im, swz, gamma);
        if (asArray) { *arrayView = v; return fallback; }
        return v;
    };
    auto& tp = g_textures[key];
    if (!tp) tp = std::make_unique<Texture>();
    Texture& t = *tp;
    bool wantDump = g_capturing && !g_dumpedTextures.count(key);
    if (!wantDump && t.valid && t.checkedFrame == g_frameNumber) return done(t);
    uint64_t hash = Fnv64(src, srcSize);
    if (levels > 1) hash ^= Fnv64(mipBase, mipTotal) * 31 + levels;
    t.checkedFrame = g_frameNumber;
    if (!wantDump && t.valid && hash == t.hash) return done(t);
    if (!t.im.img || t.im.w != w || t.im.h != h || t.im.fmt != fi.vk || t.im.mips != levels || t.im.layers != (asArray ? outD : t.im.layers)) {
        if (t.im.img) { g_deferredDestroy[g_cur].push_back(new Image(t.im)); t.im = Image(); }
        bool ok = asArray ? CreateImage(t.im, fi.vk, w, h, false, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, outD, false, true, levels)
                : dim == 2 ? CreateImage3D(t.im, fi.vk, w, h, outD)
                : dim == 3 ? CreateImage(t.im, fi.vk, w, h, false, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 6, true, false, levels)
                : CreateImage(t.im, fi.vk, w, h, false, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1, false, false, levels);
        if (!ok) return fallback;
        t.valid = false;
    }
    uint32_t outBpp = fi.convert == 0 ? fi.bytesPerBlock : 4;
    // host buffer layout: level after level, each tightly packed (slices inside a level)
    std::vector<size_t> lvOff(levels); size_t total = 0;
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t lw = std::max(w >> l, 1u), lh = std::max(h >> l, 1u);
        size_t lbw = (lw + fi.blockW - 1) / fi.blockW, lbh = (lh + fi.blockW - 1) / fi.blockW;
        lvOff[l] = total; total += ((lbw * lbh * outBpp * outD) + 15) & ~size_t(15);
    }
    EndPass();
    size_t off = RingAlloc(total, 16);
    VkCommandBuffer c = Cmd();
    uint8_t* dstAll = g_frames[g_cur].ring.ptr + off;
    uint8_t* dst = dstAll;
    for (uint32_t l = 0; l < levels; l++) {
        const LevelSrc& L = lsrc[l];
        uint32_t lw = std::max(w >> l, 1u), lh = std::max(h >> l, 1u);
        uint32_t lbw = (lw + fi.blockW - 1) / fi.blockW, lbh = (lh + fi.blockW - 1) / fi.blockW;
        size_t lslice = (size_t)lbw * lbh * outBpp;
        uint8_t* ldst = dstAll + lvOff[l];
        for (uint32_t z = 0; z < outD; z++) {
            for (uint32_t y = 0; y < lbh; y++) {
                for (uint32_t x = 0; x < lbw; x++) {
                    size_t so;
                    uint32_t sx = x + L.offX, sy = y + L.offY;
                    if (volumeData) so = tiled ? (size_t)Tiled3D((int32_t)x, (int32_t)y, (int32_t)z, pa, ha, bppLog2) : z * sliceStride + y * rowBytes + (size_t)x * fi.bytesPerBlock;
                    else so = z * L.sliceStride + (tiled ? (size_t)Tiled2D((int32_t)sx, (int32_t)sy, L.pitchBlocks, bppLog2) : sy * L.rowBytes + (size_t)sx * fi.bytesPerBlock);
                    if (so + fi.bytesPerBlock > L.size) continue;
                    uint8_t blk[16];
                    memcpy(blk, L.p + so, fi.bytesPerBlock);
                    if (fi.bytesPerBlock >= 4) for (uint32_t k = 0; k < fi.bytesPerBlock; k += 4) { uint32_t v; memcpy(&v, blk + k, 4); v = GpuSwapValue(v, endian); memcpy(blk + k, &v, 4); }
                    else if (fi.bytesPerBlock == 2 && (endian == 1 || endian == 2)) std::swap(blk[0], blk[1]);
                    uint8_t* o = ldst + z * lslice + ((size_t)y * lbw + x) * outBpp;
                    if (fi.convert == 0) memcpy(o, blk, fi.bytesPerBlock);
                    else if (fi.convert == 1) { uint16_t v = uint16_t(blk[0] | (blk[1] << 8)); for (int k = 0; k < 4; k++) o[k] = uint8_t(((v >> (4 * k)) & 0xF) * 17); }
                    else if (fi.convert == 2) { uint16_t v = uint16_t(blk[0] | (blk[1] << 8)); for (int k = 0; k < 3; k++) o[k] = uint8_t(((v >> (5 * k)) & 0x1F) * 255 / 31); o[3] = (v >> 15) ? 255 : 0; }
                    else { uint32_t v; memcpy(&v, blk, 4); float dd = float(v >> 8) / 16777215.0f; memcpy(o, &dd, 4); }
                }
            }
        }
    }
    if (wantDump) {
        g_dumpedTextures.insert(key);
        char name[160]; snprintf(name, sizeof name, "/tex_%08X_fmt%u_%ux%u%s_slot%u_m%u.ppm", base, fmt, w, h, tiled ? "" : "_linear", slot, levels);
        DumpTexture(g_captureDir + name, dst, bw, bh, w, h, fi.convert ? VK_FORMAT_R8G8B8A8_UNORM : fi.vk, outBpp);
        if (levels > 1) {   // also dump level 1 so the mip layout can be checked
            uint32_t lw = std::max(w >> 1, 1u), lh = std::max(h >> 1, 1u);
            snprintf(name, sizeof name, "/tex_%08X_fmt%u_%ux%u_slot%u_mip1.ppm", base, fmt, lw, lh, slot);
            DumpTexture(g_captureDir + name, dstAll + lvOff[1], (lw + fi.blockW - 1) / fi.blockW, (lh + fi.blockW - 1) / fi.blockW, lw, lh, fi.convert ? VK_FORMAT_R8G8B8A8_UNORM : fi.vk, outBpp);
        }
    }
    if (!t.valid) ToGeneral(c, t.im);
    std::vector<VkBufferImageCopy> bcs(levels);
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t lw = std::max(w >> l, 1u), lh = std::max(h >> l, 1u);
        VkBufferImageCopy& bc = bcs[l];
        bc.bufferOffset = off + lvOff[l]; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, dim == 3 ? 6u : asArray ? outD : 1u};
        bc.imageExtent = {lw, lh, (dim == 2 && !asArray) ? outD : 1u};
        if (fi.blockW > 1) { bc.bufferRowLength = ((lw + 3) / 4) * 4; bc.bufferImageHeight = ((lh + 3) / 4) * 4; }
    }
    vkCmdCopyBufferToImage(c, g_frames[g_cur].ring.buf, t.im.img, VK_IMAGE_LAYOUT_GENERAL, levels, bcs.data());
    FullBarrier(c);
    t.hash = hash; t.valid = true;
    return done(t);
}

VkSampler SamplerFor(uint32_t slot) {
    uint32_t d0 = Reg(R_CONST_FETCH + slot * 6), d3 = Reg(R_CONST_FETCH + slot * 6 + 3), d4 = Reg(R_CONST_FETCH + slot * 6 + 4);
    uint64_t key = ((d0 >> 10) & 0x1FF) | (((d3 >> 19) & 0x1FF) << 9) | (uint64_t((d4 >> 12) & 0x3FF) << 18);
    auto it = g_samplers.find(key);
    if (it != g_samplers.end()) return it->second;
    auto addr = [](uint32_t m) {
        switch (m) { case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT; case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
                     case 2: case 4: case 6: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; default: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; }
    };
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    uint32_t mag = (d3 >> 19) & 3, min = (d3 >> 21) & 3, aniso = (d3 >> 25) & 7;
    ci.magFilter = mag == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.minFilter = min == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    uint32_t mipf = (d3 >> 23) & 3;   // 0 point, 1 linear, 2 base map only, 3 use fetch instruction (treated as linear)
    ci.mipmapMode = mipf == 0 ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    ci.addressModeU = addr((d0 >> 10) & 7); ci.addressModeV = addr((d0 >> 13) & 7); ci.addressModeW = addr((d0 >> 16) & 7);
    ci.maxLod = mipf == 2 ? 0.0f : VK_LOD_CLAMP_NONE;
    int32_t bias = int32_t(d4 << 10) >> 22;   // signed 10-bit lod_bias at bit 12, in 1/32 steps
    static float maxBias = [] { VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(V->phys, &pr); return pr.limits.maxSamplerLodBias; }();
    ci.mipLodBias = std::clamp(bias / 32.0f, -maxBias, maxBias);
    float userAniso = (float)g_settings.anisotropy;
    if (V->aniso && (aniso || userAniso > 1) && ci.minFilter == VK_FILTER_LINEAR) { ci.anisotropyEnable = VK_TRUE; ci.maxAnisotropy = std::min(std::max(userAniso, 1.0f), V->maxAniso); }
    ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler s = VK_NULL_HANDLE; vkCreateSampler(V->dev, &ci, nullptr, &s);
    g_samplers[key] = s;
    return s;
}

// MSAA surfaces occupy samples in EDRAM: 2X doubles the rows, 4X doubles rows and columns. Render targets are
// kept in sample space so a 4X surface and a 1X surface at the same EDRAM base share one image.
void MsaaScale(uint32_t surf, uint32_t& mx, uint32_t& my) { uint32_t m = (surf >> 16) & 3; mx = m == 2 ? 2 : 1; my = m >= 1 ? 2 : 1; }

// ---------------------------------------------------------------- draws
uint32_t ScissorBottom() { return (Reg(R_WINDOW_SCISSOR_BR) >> 16) & 0x3FFF; }

void BeginPassIfNeeded(const PassState& want) {
    bool same = g_inPass && want.numColor == g_pass.numColor && want.depth == g_pass.depth;
    if (same) for (uint32_t i = 0; i < want.numColor; i++) if (want.color[i] != g_pass.color[i]) same = false;
    if (same) return;
    EndPass();
    VkRenderingAttachmentInfo ca[4]{};
    uint32_t w = UINT32_MAX, h = UINT32_MAX;
    for (uint32_t i = 0; i < want.numColor; i++) {
        ca[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; ca[i].imageView = want.color[i]->attach ? want.color[i]->attach : want.color[i]->im.view; ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        w = std::min(w, want.color[i]->im.w); h = std::min(h, want.color[i]->im.h);
    }
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (want.depth) {
        da.imageView = want.depth->im.view; da.imageLayout = VK_IMAGE_LAYOUT_GENERAL; da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        w = std::min(w, want.depth->im.w); h = std::min(h, want.depth->im.h);
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO}; ri.renderArea = {{0, 0}, {w, h}}; ri.layerCount = 1;
    ri.colorAttachmentCount = want.numColor; ri.pColorAttachments = ca;
    if (want.depth) { ri.pDepthAttachment = &da; ri.pStencilAttachment = &da; }
    vkCmdBeginRendering(Cmd(), &ri);
    g_pass = want; g_inPass = true;
}

uint64_t g_drawCount = 0, g_skipped = 0;

void EnsureRingSpace() {
    BeginFrame();
    Frame& f = g_frames[g_cur];
    if (f.ring.used > f.ring.size * 3 / 4) { SubmitFrame(true); BeginFrame(); }
}

void Draw(const GpuDrawCall& call, uint64_t vsHash, uint64_t psHash) {
    EnsureRingSpace();
    uint32_t init = call.initiator, prim = init & 0x3F, src = (init >> 6) & 3, count = init >> 16;
    bool idx32 = (init >> 11) & 1;
    if (!count || prim == 0) return;
    if (prim == 8 && !g_rectGs) { g_skipped++; return; }
    if (prim != 1 && prim != 2 && prim != 3 && prim != 4 && prim != 5 && prim != 6 && prim != 8 && prim != 13) { g_skipped++; return; }
    uint32_t edramMode = Reg(R_MODECONTROL) & 7;
    if (edramMode != 4 && edramMode != 5) return;
    Shader* vs = GetShader(vsHash, false); Shader* ps = GetShader(psHash, true);
    if (vs->failed || ps->failed || vs->t.usesMemExport) { g_skipped++; return; }

    // ---- render targets
    uint32_t surf = Reg(R_SURFACE_INFO), mx, my; MsaaScale(surf, mx, my);
    uint32_t pitch = (surf & 0x3FFF) * mx;
    uint32_t neededH = std::max<uint32_t>(ScissorBottom(), 16);
    {
        float ys = RegF(R_VPORT_XSCALE + 2), yo = RegF(R_VPORT_XSCALE + 3);
        if (Reg(R_VTE_CNTL) & 0xC) neededH = std::max<uint32_t>(neededH, (uint32_t)std::max(0.0f, std::fabs(ys) + yo));
    }
    neededH *= my;
    PassState want;
    uint32_t colorMask = Reg(R_COLOR_MASK), usedMask = 0;
    VkFormat fmts[4]{};
    if (edramMode == 5) {   // depth only: keep color 0 bound with writes off (avoids depth-only passes)
        uint32_t info = Reg(R_COLOR_INFO);
        RenderTarget* rt = GetRT(info & 0xFFF, pitch, (info >> 16) & 0xF, false, neededH);
        if (rt) { want.color[0] = rt; fmts[0] = rt->attach ? SrgbOf(rt->im.fmt) : rt->im.fmt; want.numColor = 1; }
    }
    if (edramMode == 4) {
        for (uint32_t i = 0; i < 4; i++) {
            uint32_t m = (colorMask >> (4 * i)) & 0xF;
            if (!m || !(ps->t.colorTargetsWritten & (1u << i))) { if (i == 0) { /* keep slot 0 bound for simplicity */ } else continue; }
            uint32_t info = Reg(i == 0 ? R_COLOR_INFO : R_COLOR1_INFO + i - 1);
            RenderTarget* rt = GetRT(info & 0xFFF, pitch, (info >> 16) & 0xF, false, neededH);
            if (!rt) return;
            want.color[want.numColor] = rt; fmts[want.numColor] = rt->attach ? SrgbOf(rt->im.fmt) : rt->im.fmt;
            usedMask |= (((ps->t.colorTargetsWritten >> i) & 1) ? m : 0) << (4 * want.numColor);
            want.numColor++;
            if (i == 0 && !m) {}   // write mask 0 keeps it bound without writes
        }
    }
    uint32_t depthCtl = Reg(R_DEPTHCONTROL);
    static const char* dbg = getenv("MADDEN_RENDER_DEBUG");   // n = no stencil, z = no depth test, b = no blend, s = skip depth-only draws
    if (dbg && strchr(dbg, 's') && edramMode == 5) return;
    if (dbg && strchr(dbg, 'n')) depthCtl &= ~1u;
    if (dbg && strchr(dbg, 'z')) depthCtl &= ~6u;
    bool useDepth = (depthCtl & 0x3) != 0;   // z or stencil enabled
    if (useDepth) {
        uint32_t di = Reg(R_DEPTH_INFO);
        want.depth = GetRT(di & 0xFFF, pitch, (di >> 16) & 1, true, neededH);
    }
    if (!want.numColor && !want.depth) return;
    // all attachments must be the same size for the render area; use the smallest common extent
    uint32_t rtW = UINT32_MAX, rtH = UINT32_MAX;
    for (uint32_t i = 0; i < want.numColor; i++) { rtW = std::min(rtW, want.color[i]->im.w); rtH = std::min(rtH, want.color[i]->im.h); }
    if (want.depth) { rtW = std::min(rtW, want.depth->im.w); rtH = std::min(rtH, want.depth->im.h); }

    // ---- textures (must be uploaded before the render pass starts)
    VkDescriptorImageInfo psTex[kPsTexCount]{}, vsTex[kVsTexCount]{}, psArr[kPsTexCount]{}, vsArr[kVsTexCount]{};
    size_t nps = std::min<size_t>(ps->t.textures.size(), kPsTexCount), nvs = std::min<size_t>(vs->t.textures.size(), kVsTexCount);
    for (size_t i = 0; i < nps; i++) { bool cube; VkImageView av; psTex[i].imageView = TextureView(ps->t.textures[i].constIndex, ps->t.textures[i].dimension, cube, &av);
        psTex[i].sampler = SamplerFor(ps->t.textures[i].constIndex); psTex[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL; psArr[i] = psTex[i]; psArr[i].imageView = av; }
    for (size_t i = 0; i < nvs; i++) { bool cube; VkImageView av; vsTex[i].imageView = TextureView(vs->t.textures[i].constIndex, vs->t.textures[i].dimension, cube, &av);
        vsTex[i].sampler = SamplerFor(vs->t.textures[i].constIndex); vsTex[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL; vsArr[i] = vsTex[i]; vsArr[i].imageView = av; }

    Frame& fr = g_frames[g_cur];
    // ---- vertex data
    int32_t rebase[96] = {};
    for (size_t i = 0; i < vs->t.vfetch.size() && i < 96; i++) {
        uint32_t k = vs->t.vfetch[i].constIndex;
        uint32_t f0 = Reg(R_CONST_FETCH + k * 2), f1 = Reg(R_CONST_FETCH + k * 2 + 1);
        uint32_t addr = f0 & 0x1FFFFFFC, size = ((f1 >> 2) & 0xFFFFFF) * 4;
        size = std::min<uint32_t>(size, 32u << 20);
        uint64_t ck = (uint64_t(addr) << 32) | size;
        auto it = fr.vbCache.find(ck);
        if (it != fr.vbCache.end()) { rebase[i] = (int32_t)it->second; continue; }
        size_t off = RingAlloc(size + 64, 256);
        memcpy(g_frames[g_cur].ring.ptr + off, g_base + GuestFromPhysical(addr), size);
        memset(g_frames[g_cur].ring.ptr + off + size, 0, 64);
        rebase[i] = int32_t(off / 4);
        g_frames[g_cur].vbCache[ck] = uint32_t(off / 4);
    }

    // ---- indices
    bool indexed = src == 0;
    uint32_t drawCount = count;
    size_t idxOff = 0; VkIndexType idxType = VK_INDEX_TYPE_UINT16;
    bool restart = (Reg(R_SC_MODE_CNTL) >> 21) & 1;
    if (indexed || prim == 13) {
        std::vector<uint32_t> idx;
        idx.reserve(count);
        if (indexed) {
            uint32_t endian = call.indexSizeInfo >> 30;
            const uint8_t* ip = g_base + GuestFromPhysical(call.indexBase & 0x1FFFFFFF);
            uint32_t resetIdx = Reg(R_RESET_INDX);
            for (uint32_t i = 0; i < count; i++) {
                uint32_t v;
                if (idx32) { memcpy(&v, ip + i * 4, 4); v = GpuSwapValue(v, endian); v &= 0xFFFFFF; }
                else { uint16_t s; memcpy(&s, ip + i * 2, 2); if (endian == 1 || endian == 2) s = __builtin_bswap16(s); v = s; if (restart && v == (resetIdx & 0xFFFF)) v = 0xFFFFFFFF; }
                if (idx32 && restart && v == (resetIdx & 0xFFFFFF)) v = 0xFFFFFFFF;
                idx.push_back(v);
            }
        } else for (uint32_t i = 0; i < count; i++) idx.push_back(i);
        if (prim == 13) {   // quad list -> triangle list
            std::vector<uint32_t> q; q.reserve(count / 4 * 6);
            for (uint32_t i = 0; i + 3 < idx.size(); i += 4) { uint32_t a = idx[i], b = idx[i + 1], c = idx[i + 2], d = idx[i + 3]; q.insert(q.end(), {a, b, c, a, c, d}); }
            idx.swap(q);
        }
        drawCount = (uint32_t)idx.size();
        if (!drawCount) return;
        idxType = VK_INDEX_TYPE_UINT32;
        idxOff = RingAlloc(idx.size() * 4, 4);
        memcpy(g_frames[g_cur].ring.ptr + idxOff, idx.data(), idx.size() * 4);
        indexed = true;
    }

    // ---- constants
    size_t uboAlign = std::max<size_t>(g_props.limits.minUniformBufferOffsetAlignment, 16);
    size_t uboOff = RingAlloc(kUboSize, uboAlign);
    uint8_t* u = g_frames[g_cur].ring.ptr + uboOff;
    for (uint32_t i = 0; i < 2048; i++) { uint32_t v = Reg(R_CONST_FLOAT + i); memcpy(u + i * 4, &v, 4); }
    uint8_t* p = u + 512 * 16;
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t v = Reg(R_CONST_BOOL + i);
        // Replay/cutscene depth-of-field composite: bools 128 (DOF blend) and 129 (glow add) gate the blur paths.
        if (i == 4 && psHash == 0xddf46ef217b6b95bull && !g_settings.depthOfField) v &= ~3u;
        memcpy(p + i * 4, &v, 4);
    }
    p += 32;
    for (uint32_t i = 0; i < 32; i++) { uint32_t v = Reg(R_CONST_LOOP + i); memcpy(p + i * 4, &v, 4); }
    p += 128;
    for (uint32_t i = 0; i < 192; i++) { uint32_t v = Reg(R_CONST_FETCH + i); memcpy(p + i * 4, &v, 4); }
    p += 768;
    // viewport -> host NDC
    uint32_t vte = Reg(R_VTE_CNTL), mode = Reg(R_SC_MODE_CNTL);
    float sx = (vte & 1) ? RegF(R_VPORT_XSCALE) : 1.0f, ox = (vte & 2) ? RegF(R_VPORT_XSCALE + 1) : 0.0f;
    float sy = (vte & 4) ? RegF(R_VPORT_XSCALE + 2) : 1.0f, oy = (vte & 8) ? RegF(R_VPORT_XSCALE + 3) : 0.0f;
    float sz = (vte & 16) ? RegF(R_VPORT_XSCALE + 4) : 1.0f, oz = (vte & 32) ? RegF(R_VPORT_XSCALE + 5) : 0.0f;
    if ((mode >> 16) & 1) {
        uint32_t wo = Reg(R_WINDOW_OFFSET);
        ox += float(int32_t(wo << 17) >> 17); oy += float(int32_t((wo >> 16) << 17) >> 17);
    }
    if (!(Reg(R_VTX_CNTL) & 1)) { ox += 0.5f; oy += 0.5f; }   // D3D pixel centers
    float gw = float(rtW) / (S * mx), gh = float(rtH) / (S * my);
    float ndc[8] = {sx * 2.0f / gw, sy * 2.0f / gh, sz, 0, ox * 2.0f / gw - 1.0f, oy * 2.0f / gh - 1.0f, oz, 0};
    memcpy(p, ndc, 32); p += 32;
    int32_t paramGen = (Reg(R_SQ_PROGRAM_CNTL) >> 18) & 1 ? int32_t((Reg(R_SQ_CONTEXT_MISC) >> 8) & 0xFF) : -1;
    uint32_t cc = Reg(R_COLORCONTROL);
    int32_t alphaFunc = (cc & 8) ? int32_t(cc & 7) : 7;
    int32_t clipOff = (((Reg(0x2204) >> 16) & 1) || prim == 8 || prim == 13) ? 1 : 0;   // clip_disable, or screen-space rects
    int32_t misc[4] = {(int32_t)vte, paramGen, alphaFunc, clipOff}; memcpy(p, misc, 16); p += 16;
    float aref[4] = {RegF(R_ALPHA_REF), 0, 0, 0}; memcpy(p, aref, 16); p += 16;
    float expScale[4];
    for (uint32_t i = 0; i < 4; i++) { uint32_t ci = Reg(i == 0 ? R_COLOR_INFO : R_COLOR1_INFO + i - 1); expScale[i] = std::ldexp(1.0f, int32_t(ci << 6) >> 26); }
    memcpy(p, expScale, 16); p += 16;
    memcpy(p, rebase, sizeof rebase);

    // ---- pipeline
    PipeKey k{};
    k.vs = vsHash; k.ps = psHash; k.prim = prim; k.numColor = want.numColor;
    for (uint32_t i = 0; i < want.numColor; i++) k.colorFmts |= uint32_t(fmts[i] & 0xFF) << (8 * i);
    k.depthFmt = want.depth ? 1 : 0; k.colorMask = usedMask;
    for (uint32_t i = 0; i < 4; i++) k.blend[i] = (dbg && strchr(dbg, 'b')) ? 0x00010001u : Reg(i == 0 ? R_BLENDCONTROL0 : R_BLENDCONTROL1 + i - 1);
    k.depthCtl = useDepth ? depthCtl : 0; k.mode = mode & 7; k.restart = restart;
    VkPipeline pipe = GetPipeline(k, vs, ps, fmts, want.depth != nullptr);
    if (!pipe) { g_skipped++; return; }

    // ---- descriptors
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = g_frames[g_cur].dpool; dai.descriptorSetCount = 1; dai.pSetLayouts = &g_setLayout;
    VkDescriptorSet set;
    if (vkAllocateDescriptorSets(V->dev, &dai, &set) != VK_SUCCESS) {
        SubmitFrame(true); BeginFrame();   // pool exhausted: start over in a fresh frame slot state
        dai.descriptorPool = g_frames[g_cur].dpool;
        if (vkAllocateDescriptorSets(V->dev, &dai, &set) != VK_SUCCESS) return;
        return;   // ring contents were reset; skip this draw
    }
    VkDescriptorBufferInfo ubi{g_frames[g_cur].ring.buf, 0, kUboSize}, sbi{g_frames[g_cur].ring.buf, 0, VK_WHOLE_SIZE};
    std::vector<VkWriteDescriptorSet> wr;
    wr.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, nullptr, &ubi, nullptr});
    if (!vs->t.vfetch.empty()) wr.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &sbi, nullptr});
    for (size_t i = 0; i < nps; i++) wr.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, kPsTexBase + (uint32_t)i, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &psTex[i], nullptr, nullptr});
    for (size_t i = 0; i < nvs; i++) wr.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, kVsTexBase + (uint32_t)i, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &vsTex[i], nullptr, nullptr});
    for (size_t i = 0; i < nps; i++) if (ps->t.textures[i].dimension == 2) wr.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 24 + (uint32_t)i, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &psArr[i], nullptr, nullptr});
    for (size_t i = 0; i < nvs; i++) if (vs->t.textures[i].dimension == 2) wr.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 48 + (uint32_t)i, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &vsArr[i], nullptr, nullptr});
    vkUpdateDescriptorSets(V->dev, (uint32_t)wr.size(), wr.data(), 0, nullptr);

    if (Tracing()) {
        if (g_capturing) { g_captureShaders.push_back(vsHash); g_captureShaders.push_back(psHash); }
        fprintf(g_traceOut, "[trace] draw prim %u count %u src %u vs %016llx ps %016llx colors %u", prim, count, src, (unsigned long long)vsHash, (unsigned long long)psHash, want.numColor);
        for (uint32_t i = 0; i < want.numColor; i++) fprintf(g_traceOut, " [base %u pitch %u fmt %u]", want.color[i]->base, want.color[i]->pitch, want.color[i]->format);
        if (want.depth) fprintf(g_traceOut, " depth[base %u ctl %08X]", want.depth->base, depthCtl);
        fprintf(g_traceOut, " cc %08X woff %08X mode %08X vtx %08X", Reg(R_COLORCONTROL), Reg(R_WINDOW_OFFSET), Reg(R_SC_MODE_CNTL), Reg(R_VTX_CNTL));
        if (prim == 8 && !vs->t.vfetch.empty()) {   // rect lists: first floats of the first vertex stream
            uint32_t k0 = vs->t.vfetch[0].constIndex, a0 = Reg(R_CONST_FETCH + k0 * 2) & 0x1FFFFFFC, e0 = Reg(R_CONST_FETCH + k0 * 2 + 1) & 3;
            fprintf(g_traceOut, " v[");
            for (int q = 0; q < 21; q++) { uint32_t raw; memcpy(&raw, g_base + GuestFromPhysical(a0) + q * 4, 4); raw = GpuSwapValue(raw, e0); float fv; memcpy(&fv, &raw, 4); fprintf(g_traceOut, "%s%g", q ? " " : "", fv); }
            fprintf(g_traceOut, "]");
        }
        fprintf(g_traceOut, " msaa %u srm %08X srmbf %08X scissor %08X-%08X expbias %d", (Reg(R_SURFACE_INFO) >> 16) & 3, Reg(R_STENCILREFMASK), Reg(R_STENCILREFMASK_BF), Reg(R_WINDOW_SCISSOR_TL), Reg(R_WINDOW_SCISSOR_BR), int32_t(Reg(R_COLOR_INFO) << 6) >> 26);
        for (size_t i = 0; i < nps; i++) {
            uint32_t sl = ps->t.textures[i].constIndex, t1 = Reg(R_CONST_FETCH + sl * 6 + 1), t2 = Reg(R_CONST_FETCH + sl * 6 + 2), t0 = Reg(R_CONST_FETCH + sl * 6);
            { const uint8_t* tp = g_base + GuestFromPhysical(t1 & 0xFFFFF000); uint64_t sum = 0; for (int q = 0; q < 65536; q++) sum += tp[q]; fprintf(g_traceOut, " avg %.1f", sum / 65536.0); }
            fprintf(g_traceOut, " fc{%08X %08X %08X %08X %08X %08X}", Reg(R_CONST_FETCH + sl * 6), t1, t2, Reg(R_CONST_FETCH + sl * 6 + 3), Reg(R_CONST_FETCH + sl * 6 + 4), Reg(R_CONST_FETCH + sl * 6 + 5));
            fprintf(g_traceOut, " tex%u[fmt %u %ux%u tiled %u base %08X dim %u%s]", sl, t1 & 0x3F, (t2 & 0x1FFF) + 1, ((t2 >> 13) & 0x1FFF) + 1, t0 >> 31, t1 & 0xFFFFF000,
                    ps->t.textures[i].dimension, g_resolved.count(t1 & 0xFFFFF000) ? " resolved" : "");
        }
        fprintf(g_traceOut, " vte %03X scale %.1f %.1f off %.1f %.1f mask %04X blend %08X tex %zu\n", vte, sx, sy, ox, oy, usedMask, k.blend[0], nps);
    }
    // ---- record
    BeginPassIfNeeded(want);
    VkCommandBuffer c = Cmd();
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    uint32_t dynOff = (uint32_t)uboOff;
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeLayout, 0, 1, &set, 1, &dynOff);
    VkViewport vp{0, 0, float(rtW), float(rtH), 0.0f, 1.0f};
    vkCmdSetViewport(c, 0, 1, &vp);
    uint32_t tl = Reg(R_WINDOW_SCISSOR_TL), br = Reg(R_WINDOW_SCISSOR_BR);
    int32_t x0 = tl & 0x3FFF, y0 = (tl >> 16) & 0x3FFF, x1 = br & 0x3FFF, y1 = (br >> 16) & 0x3FFF;
    if (!(tl >> 31)) { uint32_t wo = Reg(R_WINDOW_OFFSET); int32_t wx = int32_t(wo << 17) >> 17, wy = int32_t((wo >> 16) << 17) >> 17; x0 += wx; x1 += wx; y0 += wy; y1 += wy; }
    x0 = std::max(x0, 0); y0 = std::max(y0, 0);
    int32_t sxs = int32_t(S * mx), sys = int32_t(S * my);
    VkRect2D sc{{x0 * sxs, y0 * sys}, {uint32_t(std::max(0, x1 - x0) * sxs), uint32_t(std::max(0, y1 - y0) * sys)}};
    sc.extent.width = std::min<uint32_t>(sc.extent.width, rtW > (uint32_t)sc.offset.x ? rtW - sc.offset.x : 0);
    sc.extent.height = std::min<uint32_t>(sc.extent.height, rtH > (uint32_t)sc.offset.y ? rtH - sc.offset.y : 0);
    if (!sc.extent.width || !sc.extent.height) return;
    vkCmdSetScissor(c, 0, 1, &sc);
    float bc[4] = {RegF(R_BLEND_RED), RegF(R_BLEND_RED + 1), RegF(R_BLEND_RED + 2), RegF(R_BLEND_RED + 3)};
    vkCmdSetBlendConstants(c, bc);
    uint32_t srm = Reg(R_STENCILREFMASK), srmb = ((depthCtl >> 7) & 1) ? Reg(R_STENCILREFMASK_BF) : srm;
    vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_BIT, srm & 0xFF); vkCmdSetStencilReference(c, VK_STENCIL_FACE_BACK_BIT, srmb & 0xFF);
    vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_BIT, (srm >> 8) & 0xFF); vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_BACK_BIT, (srmb >> 8) & 0xFF);
    vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_BIT, (srm >> 16) & 0xFF); vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_BACK_BIT, (srmb >> 16) & 0xFF);
    int32_t indexOffset = (int32_t)(Reg(R_INDX_OFFSET) & 0xFFFFFF);
    if (indexed) {
        vkCmdBindIndexBuffer(c, g_frames[g_cur].ring.buf, idxOff, idxType);
        vkCmdDrawIndexed(c, drawCount, 1, 0, indexOffset, 0);
    } else vkCmdDraw(c, drawCount, 1, (uint32_t)indexOffset, 0);
    g_drawCount++;

    // EDRAM aliasing trick used for fast clears: a depth/stencil clear rect whose depth target shares its EDRAM base
    // with the color target also overwrites the color (same memory on the console). Mirror that on the color image.
    if (prim == 8 && want.depth && want.numColor && !(usedMask & 0xF) && (depthCtl & 4) && ((depthCtl >> 4) & 7) == 7 &&
        (Reg(R_DEPTH_INFO) & 0xFFF) == (Reg(R_COLOR_INFO) & 0xFFF) && !vs->t.vfetch.empty()) {
        uint32_t k0 = vs->t.vfetch[0].constIndex, a0 = Reg(R_CONST_FETCH + k0 * 2) & 0x1FFFFFFC, e0 = Reg(R_CONST_FETCH + k0 * 2 + 1) & 3;
        uint32_t st = std::max<uint32_t>(vs->t.firstFetchStride, 2), of = vs->t.firstFetchOffset;
        float vx[3], vy[3], vz = 0;
        for (int q = 0; q < 3; q++) {
            uint32_t raw[3];
            for (int j = 0; j < 3; j++) { memcpy(&raw[j], g_base + GuestFromPhysical(a0) + (q * st + of + j) * 4, 4); raw[j] = GpuSwapValue(raw[j], e0); }
            memcpy(&vx[q], &raw[0], 4); memcpy(&vy[q], &raw[1], 4); if (q == 0) memcpy(&vz, &raw[2], 4);
        }
        float minx = std::min({vx[0], vx[1], vx[2]}), maxx = std::max({vx[0], vx[1], vx[2]});
        float miny = std::min({vy[0], vy[1], vy[2]}), maxy = std::max({vy[0], vy[1], vy[2]});
        if (std::isfinite(minx) && std::isfinite(maxy) && maxx > minx && maxy > miny) {
            int32_t cx0 = int32_t(std::max(0.0f, minx)) * sxs, cy0 = int32_t(std::max(0.0f, miny)) * sys;
            uint32_t cw = uint32_t(std::max(0.0f, maxx - std::max(0.0f, minx))) * sxs, ch = uint32_t(std::max(0.0f, maxy - std::max(0.0f, miny))) * sys;
            cw = std::min<uint32_t>(cw, rtW > (uint32_t)cx0 ? rtW - cx0 : 0); ch = std::min<uint32_t>(ch, rtH > (uint32_t)cy0 ? rtH - cy0 : 0);
            if (cw && ch) {
                // the depth/stencil bits as they land in a 32-bit color pixel: depth24 << 8 | stencil
                uint32_t bits = (uint32_t(std::clamp(vz, 0.0f, 1.0f) * 16777215.0f) << 8) | (((depthCtl & 1) && ((depthCtl >> 14) & 7) == 2) ? (srm & 0xFF) : 0);
                VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
                for (int k = 0; k < 4; k++) ca.clearValue.color.float32[k] = ((bits >> (8 * k)) & 0xFF) / 255.0f;
                VkClearRect cr{{{cx0, cy0}, {cw, ch}}, 0, 1};
                vkCmdClearAttachments(c, 1, &ca, 1, &cr);
                if (Tracing()) fprintf(g_traceOut, "[trace]   -> aliased depth clear also cleared color %d,%d %ux%u to %08X\n", cx0, cy0, cw, ch, bits);
            }
        }
    }
}

// ---------------------------------------------------------------- capture helpers (F12)
std::string ExeDir() {
#ifdef _WIN32
    char b[1024]; DWORD n = GetModuleFileNameA(nullptr, b, sizeof b); std::string p(b, n);
#else
    std::string p = std::filesystem::read_symlink("/proc/self/exe").string();
#endif
    size_t k = p.find_last_of("/\\"); return k == std::string::npos ? "." : p.substr(0, k);
}
float HalfToFloat(uint16_t h) { uint32_t e = (h >> 10) & 31, m = h & 1023, sgn = h >> 15; float v = e == 0 ? m / 1024.0f / 16384.0f : e == 31 ? 65504.0f : std::ldexp(1.0f + m / 1024.0f, int(e) - 15); return sgn ? -v : v; }
// Reads an image back and writes it as a PPM (downscaled to guest resolution). Submits and waits.
void DumpImage(Image& im, uint32_t w, uint32_t h, const std::string& path) {
    if (im.depth || !im.img) return;
    uint32_t bpp = 4;
    switch (im.fmt) { case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_SNORM: case VK_FORMAT_R32G32_SFLOAT: bpp = 8; break; default: break; }
    w = std::min(w, im.w); h = std::min(h, im.h);
    EndPass();
    size_t off = RingAlloc((size_t)w * h * bpp, 256);
    VkBufferImageCopy bc{}; bc.bufferOffset = off; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bc.imageExtent = {w, h, 1};
    FullBarrier(Cmd());
    vkCmdCopyImageToBuffer(Cmd(), im.img, VK_IMAGE_LAYOUT_GENERAL, g_frames[g_cur].ring.buf, 1, &bc);
    SubmitFrame(true);
    const uint8_t* px = g_frames[g_cur].ring.ptr + off;
    uint32_t ow = w / S, oh = h / S;
    FILE* f = fopen(path.c_str(), "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", ow, oh);
        std::vector<uint8_t> row(ow * 3);
        for (uint32_t y = 0; y < oh; y++) {
            for (uint32_t x = 0; x < ow; x++) {
                const uint8_t* p = px + ((size_t)(y * S) * w + x * S) * bpp;
                float c[3];
                switch (im.fmt) {
                case VK_FORMAT_A2B10G10R10_UNORM_PACK32: { uint32_t v; memcpy(&v, p, 4); for (int k = 0; k < 3; k++) c[k] = ((v >> (10 * k)) & 1023) / 1023.0f; break; }
                case VK_FORMAT_R16G16B16A16_SFLOAT: { uint16_t v[4]; memcpy(v, p, 8); for (int k = 0; k < 3; k++) c[k] = HalfToFloat(v[k]); break; }
                case VK_FORMAT_R16G16_SFLOAT: { uint16_t v[2]; memcpy(v, p, 4); c[0] = HalfToFloat(v[0]); c[1] = HalfToFloat(v[1]); c[2] = 0; break; }
                case VK_FORMAT_R32_SFLOAT: { float v; memcpy(&v, p, 4); c[0] = c[1] = c[2] = v; break; }
                case VK_FORMAT_R32G32_SFLOAT: { float v[2]; memcpy(v, p, 8); c[0] = v[0]; c[1] = v[1]; c[2] = 0; break; }
                default: for (int k = 0; k < 3; k++) c[k] = p[k] / 255.0f; break;
                }
                for (int k = 0; k < 3; k++) row[x * 3 + k] = (uint8_t)std::clamp(c[k] * 255.0f + 0.5f, 0.0f, 255.0f);
            }
            fwrite(row.data(), 1, row.size(), f);
        }
        fclose(f);
    }
    BeginFrame();
}

// ---------------------------------------------------------------- resolves
void Resolve() {
    EnsureRingSpace();
    uint32_t cc = Reg(R_COPY_CONTROL);
    uint32_t srcSel = cc & 7, cmd = (cc >> 20) & 3;
    bool clearColor = (cc >> 8) & 1, clearDepth = (cc >> 9) & 1;
    uint32_t surf = Reg(R_SURFACE_INFO), mx, my; MsaaScale(surf, mx, my);
    uint32_t pitch = surf & 0x3FFF;
    // resolve rectangle from vertex fetch constant 0 (3 screen-space vertices)
    uint32_t f0 = Reg(R_CONST_FETCH), f1 = Reg(R_CONST_FETCH + 1);
    const uint8_t* vp = g_base + GuestFromPhysical(f0 & 0x1FFFFFFC);
    float v[6];
    for (int i = 0; i < 6; i++) { uint32_t raw; memcpy(&raw, vp + i * 4, 4); raw = GpuSwapValue(raw, f1 & 3); memcpy(&v[i], &raw, 4); }
    float half = (Reg(R_VTX_CNTL) & 1) ? 0.0f : 0.5f;
    auto fx = [&](float a) { return int32_t(std::floor(a + half + 0.5f - 1.0f / 512)); };
    int32_t x0 = std::min({fx(v[0]), fx(v[2]), fx(v[4])}), y0 = std::min({fx(v[1]), fx(v[3]), fx(v[5])});
    int32_t x1 = std::max({fx(v[0]), fx(v[2]), fx(v[4])}), y1 = std::max({fx(v[1]), fx(v[3]), fx(v[5])});
    if ((Reg(R_SC_MODE_CNTL) >> 16) & 1) {
        uint32_t wo = Reg(R_WINDOW_OFFSET); int32_t wx = int32_t(wo << 17) >> 17, wy = int32_t((wo >> 16) << 17) >> 17;
        x0 += wx; x1 += wx; y0 += wy; y1 += wy;
    }
    uint32_t tl = Reg(R_WINDOW_SCISSOR_TL), br = Reg(R_WINDOW_SCISSOR_BR);
    x0 = std::clamp<int32_t>(x0, tl & 0x3FFF, br & 0x3FFF); x1 = std::clamp<int32_t>(x1, tl & 0x3FFF, br & 0x3FFF);
    y0 = std::clamp<int32_t>(y0, (tl >> 16) & 0x3FFF, (br >> 16) & 0x3FFF); y1 = std::clamp<int32_t>(y1, (tl >> 16) & 0x3FFF, (br >> 16) & 0x3FFF);
    x0 &= ~7; y0 &= ~7;
    x1 = std::min<int32_t>(x1, (int32_t)pitch);
    if (Tracing()) fprintf(g_traceOut, "[trace] resolve src %u rect %d,%d-%d,%d dest %08X pitch %u h %u fmt %u clear %d%d cmd %u depthclear %08X srcbase %u pitch %u msaa %u destexpbias %d rtexpbias %d\n", srcSel, x0, y0, x1, y1,
                           Reg(R_COPY_DEST_BASE), Reg(R_COPY_DEST_PITCH) & 0x3FFF, (Reg(R_COPY_DEST_PITCH) >> 16) & 0x3FFF, (Reg(R_COPY_DEST_INFO) >> 7) & 0x3F, clearColor, clearDepth, cmd, Reg(R_DEPTH_CLEAR), Reg(R_COLOR_INFO) & 0xFFF, Reg(R_SURFACE_INFO) & 0x3FFF, (Reg(R_SURFACE_INFO) >> 16) & 3, int32_t(Reg(R_COPY_DEST_INFO) << 10) >> 26, int32_t(Reg(R_COLOR_INFO) << 6) >> 26);
    if (x1 <= x0 || y1 <= y0) return;

    bool isDepth = srcSel == 4;
    RenderTarget* rt = nullptr;
    if (isDepth) { uint32_t di = Reg(R_DEPTH_INFO); rt = GetRT(di & 0xFFF, pitch * mx, (di >> 16) & 1, true, (uint32_t)y1 * my); }
    else { uint32_t info = Reg(srcSel == 0 ? R_COLOR_INFO : R_COLOR1_INFO + srcSel - 1); rt = GetRT(info & 0xFFF, pitch * mx, (info >> 16) & 0xF, false, (uint32_t)y1 * my); }
    EndPass();
    VkCommandBuffer c = Cmd();
    uint32_t destBase = Reg(R_COPY_DEST_BASE) & 0x1FFFFFFF, dp = Reg(R_COPY_DEST_PITCH), di = Reg(R_COPY_DEST_INFO);
    uint32_t destPitch = dp & 0x3FFF, destHeight = (dp >> 16) & 0x3FFF, destFormat = (di >> 7) & 0x3F;
    if (rt && !isDepth && (cmd == 0 || cmd == 1) && destPitch && destHeight) {
        uint32_t w = std::max<uint32_t>(destPitch, (uint32_t)x1), h = std::max<uint32_t>(destHeight, (uint32_t)y1);
        auto& rp = g_resolved[destBase];
        if (!rp || rp->im.w < w * S || rp->im.h < h * S || rp->im.fmt != rt->im.fmt) {
            if (rp) g_deferredDestroy[g_cur].push_back(new Image(rp->im));
            rp = std::make_unique<Resolved>();
            if (!CreateImage(rp->im, rt->im.fmt, w * S, h * S, false, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) { g_resolved.erase(destBase); return; }
            ToGeneral(c, rp->im);
            VkClearColorValue z{}; VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(c, rp->im.img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &r);
            FullBarrier(c);
            if (g_log) fprintf(stderr, "[render] resolve target %08X %ux%u format %u\n", destBase, w, h, destFormat);
        }
        rp->base = destBase; rp->pitch = destPitch; rp->height = destHeight; rp->format = destFormat; rp->frame = g_frameNumber;
        rp->swapRB = (di >> 24) & 1;
        // source in sample space, destination in pixels
        uint32_t cw = std::min<uint32_t>((x1 - x0) * S, rp->im.w - x0 * S), ch = std::min<uint32_t>((y1 - y0) * S, rp->im.h - y0 * S);
        uint32_t sx0 = x0 * S * mx, sy0 = y0 * S * my;
        uint32_t scw = std::min<uint32_t>(cw * mx, rt->im.w - std::min<uint32_t>(rt->im.w, sx0)), sch = std::min<uint32_t>(ch * my, rt->im.h - std::min<uint32_t>(rt->im.h, sy0));
        int32_t destBias = int32_t(di << 10) >> 26;   // RB_COPY_DEST_INFO.copy_dest_exp_bias
        if (cw && ch && scw && sch && destBias != 0) {
            ScaledCopy(rt->im, VkRect2D{{(int32_t)sx0, (int32_t)sy0}, {scw, sch}}, rp->im,
                       VkRect2D{{x0 * (int32_t)S, y0 * (int32_t)S}, {scw / mx, sch / my}}, std::ldexp(1.0f, destBias));
        } else if (cw && ch && scw && sch) {
            if (mx == 1 && my == 1) {
                VkImageCopy ic{}; ic.srcSubresource = ic.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                ic.srcOffset = ic.dstOffset = {x0 * (int32_t)S, y0 * (int32_t)S, 0}; ic.extent = {std::min(cw, scw), std::min(ch, sch), 1};
                vkCmdCopyImage(c, rt->im.img, VK_IMAGE_LAYOUT_GENERAL, rp->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
            } else {   // MSAA resolve: average the samples with a linear downscale
                VkImageBlit bl{}; bl.srcSubresource = bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                bl.srcOffsets[0] = {(int32_t)sx0, (int32_t)sy0, 0}; bl.srcOffsets[1] = {(int32_t)(sx0 + scw), (int32_t)(sy0 + sch), 1};
                bl.dstOffsets[0] = {x0 * (int32_t)S, y0 * (int32_t)S, 0}; bl.dstOffsets[1] = {x0 * (int32_t)S + (int32_t)(scw / mx), y0 * (int32_t)S + (int32_t)(sch / my), 1};
                vkCmdBlitImage(c, rt->im.img, VK_IMAGE_LAYOUT_GENERAL, rp->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
            }
            FullBarrier(c);
        }
        if (g_capturing) {
            char name[128]; snprintf(name, sizeof name, "/%03d_resolve_%08X_%ux%u.ppm", g_captureSeq++, destBase, destPitch, destHeight);
            DumpImage(rp->im, std::max<uint32_t>(destPitch, (uint32_t)x1) * S, std::max<uint32_t>(destHeight, (uint32_t)y1) * S, g_captureDir + name);
            fprintf(g_traceOut, "[trace]   -> saved %s (swapRB %d)\n", name + 1, (int)rp->swapRB);
        }
    } else if (rt && isDepth && destPitch && destHeight) {
        // Depth resolve: depth aspect -> buffer -> R32F image, sampled later as a 24_8 / 24_8_FLOAT texture.
        // Kept in the render target's sample space (normalized sampling hides the difference).
        if (g_depthFormat != VK_FORMAT_D32_SFLOAT_S8_UINT) {
            static bool warned = false; if (!warned) { warned = true; fprintf(stderr, "[render] depth resolves need D32S8 - skipped\n"); }
        } else {
            uint32_t w = std::max<uint32_t>(destPitch, (uint32_t)x1) * S * mx, h = std::max<uint32_t>(destHeight, (uint32_t)y1) * S * my;
            auto& rp = g_resolved[destBase];
            if (!rp || rp->im.w != w || rp->im.h != h || rp->im.fmt != VK_FORMAT_R32_SFLOAT) {
                if (rp) g_deferredDestroy[g_cur].push_back(new Image(rp->im));
                rp = std::make_unique<Resolved>();
                if (!CreateImage(rp->im, VK_FORMAT_R32_SFLOAT, w, h, false, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) { g_resolved.erase(destBase); return; }
                ToGeneral(Cmd(), rp->im);
                VkClearColorValue one{{1, 1, 1, 1}}; VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdClearColorImage(Cmd(), rp->im.img, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &r);
                FullBarrier(Cmd());
                if (g_log) fprintf(stderr, "[render] depth resolve target %08X %ux%u\n", destBase, w, h);
            }
            rp->base = destBase; rp->pitch = destPitch; rp->height = destHeight; rp->format = destFormat; rp->frame = g_frameNumber; rp->swapRB = false;
            uint32_t sx0 = x0 * S * mx, sy0 = y0 * S * my;
            uint32_t cw = std::min<uint32_t>((x1 - x0) * S * mx, std::min(rt->im.w - std::min(rt->im.w, sx0), w - std::min(w, sx0)));
            uint32_t ch = std::min<uint32_t>((y1 - y0) * S * my, std::min(rt->im.h - std::min(rt->im.h, sy0), h - std::min(h, sy0)));
            if (cw && ch) {
                size_t off = RingAlloc((size_t)cw * ch * 4, 256);
                VkCommandBuffer cc = Cmd();
                VkBufferImageCopy bc{}; bc.bufferOffset = off; bc.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
                bc.imageOffset = {(int32_t)sx0, (int32_t)sy0, 0}; bc.imageExtent = {cw, ch, 1};
                vkCmdCopyImageToBuffer(cc, rt->im.img, VK_IMAGE_LAYOUT_GENERAL, g_frames[g_cur].ring.buf, 1, &bc);
                FullBarrier(cc);
                bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                vkCmdCopyBufferToImage(cc, g_frames[g_cur].ring.buf, rp->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
                FullBarrier(cc);
            }
            if (g_capturing) {
                char name[128]; snprintf(name, sizeof name, "/%03d_depth_resolve_%08X_%ux%u.ppm", g_captureSeq++, destBase, destPitch, destHeight);
                DumpImage(rp->im, w, h, g_captureDir + name);
                fprintf(g_traceOut, "[trace]   -> saved %s\n", name + 1);
            }
        }
    }
    // clears that come with the resolve
    VkClearRect cr{{{x0 * (int32_t)(S * mx), y0 * (int32_t)(S * my)}, {uint32_t(x1 - x0) * S * mx, uint32_t(y1 - y0) * S * my}}, 0, 1};
    if (clearColor && !isDepth && rt) {
        uint32_t cv = Reg(R_COLOR_CLEAR);
        VkClearColorValue col{};
        if (rt->im.fmt == VK_FORMAT_R8G8B8A8_UNORM || rt->im.fmt == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
            // RB_COLOR_CLEAR holds ARGB8888
            col.float32[0] = ((cv >> 16) & 0xFF) / 255.0f; col.float32[1] = ((cv >> 8) & 0xFF) / 255.0f; col.float32[2] = (cv & 0xFF) / 255.0f; col.float32[3] = (cv >> 24) / 255.0f;
        } else {
            float fcv; memcpy(&fcv, &cv, 4); col.float32[0] = col.float32[1] = col.float32[2] = col.float32[3] = 0.0f; (void)fcv;
        }
        PassState ps; ps.color[0] = rt; ps.numColor = 1;
        BeginPassIfNeeded(ps);
        VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}}; ca.clearValue.color = col;
        cr.rect.extent.width = std::min(cr.rect.extent.width, rt->im.w - std::min<uint32_t>(rt->im.w, cr.rect.offset.x));
        cr.rect.extent.height = std::min(cr.rect.extent.height, rt->im.h - std::min<uint32_t>(rt->im.h, cr.rect.offset.y));
        if (cr.rect.extent.width && cr.rect.extent.height) vkCmdClearAttachments(Cmd(), 1, &ca, 1, &cr);
    }
    if (clearDepth) {
        uint32_t di2 = Reg(R_DEPTH_INFO);
        RenderTarget* d = GetRT(di2 & 0xFFF, pitch * mx, (di2 >> 16) & 1, true, (uint32_t)y1 * my);
        if (d) {
            uint32_t dv = Reg(R_DEPTH_CLEAR);
            float z = ((di2 >> 16) & 1) ? 1.0f : float(dv >> 8) / 16777215.0f;
            if ((di2 >> 16) & 1) { uint32_t m = dv >> 8; (void)m; z = 1.0f; }
            PassState ps; ps.depth = d;
            BeginPassIfNeeded(ps);
            VkClearAttachment ca{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, {}};
            ca.clearValue.depthStencil = {z, dv & 0xFF};
            VkClearRect dr = cr;
            dr.rect.extent.width = std::min(dr.rect.extent.width, d->im.w - std::min<uint32_t>(d->im.w, dr.rect.offset.x));
            dr.rect.extent.height = std::min(dr.rect.extent.height, d->im.h - std::min<uint32_t>(d->im.h, dr.rect.offset.y));
            if (dr.rect.extent.width && dr.rect.extent.height) vkCmdClearAttachments(Cmd(), 1, &ca, 1, &dr);
        }
    }
    EndPass();
}

// ---------------------------------------------------------------- presentation
Image g_present[2];

bool Tracing() { return g_capturing || (g_frameNumber >= g_traceFrame && g_frameNumber < g_traceFrame + 8); }
void FinishCapture() {
    if (!g_capturing) return;
    // shaders used in the frame, as translated GLSL
    std::sort(g_captureShaders.begin(), g_captureShaders.end());
    g_captureShaders.erase(std::unique(g_captureShaders.begin(), g_captureShaders.end()), g_captureShaders.end());
    for (uint64_t h : g_captureShaders) {
        auto it = g_shaders.find(h);
        if (it == g_shaders.end()) continue;
        char name[128]; snprintf(name, sizeof name, "/shader_%016llx.%s.glsl", (unsigned long long)h, it->second->t.stage == xenos::ShaderStage::kPixel ? "ps" : "vs");
        if (FILE* f = fopen((g_captureDir + name).c_str(), "w")) { fputs(it->second->t.glsl.c_str(), f); fclose(f); }
    }
    fclose(g_traceOut); g_traceOut = stderr;
    g_capturing = false;
    fprintf(stderr, "[render] capture saved to %s\n", g_captureDir.c_str());
}
void StartCapture() {
    static int n = 0;
    char d[64];
    do { snprintf(d, sizeof d, "/capture_%02d", ++n); g_captureDir = ExeDir() + d; } while (std::filesystem::exists(g_captureDir) && n < 99);
    std::error_code ec; std::filesystem::create_directories(g_captureDir, ec);
    FILE* f = fopen((g_captureDir + "/trace.txt").c_str(), "w");
    if (!f) { fprintf(stderr, "[render] could not create %s\n", g_captureDir.c_str()); return; }
    g_traceOut = f; g_capturing = true; g_captureSeq = 0; g_captureShaders.clear(); g_dumpedTextures.clear();
    fprintf(stderr, "[render] F12: capturing the next frame into %s\n", g_captureDir.c_str());
}
bool SwapInner(uint32_t fb, uint32_t w, uint32_t h, const uint32_t fetch[6]);
bool Swap(uint32_t fb, uint32_t w, uint32_t h, const uint32_t fetch[6]) {
    SavePipelineCache(false);
    bool ok = SwapInner(fb, w, h, fetch);
    if (g_capturing) FinishCapture();   // frame ended without a GPU front buffer
    if (g_captureRequest.exchange(false)) StartCapture();
    return ok;
}
bool SwapInner(uint32_t fb, uint32_t w, uint32_t h, const uint32_t fetch[6]) {
    bool finishing = g_capturing;
    if (Tracing()) fprintf(g_traceOut, "[trace] swap fb %08X %ux%u resolved %d\n", fb, w, h, (int)g_resolved.count(fb & 0x1FFFFFFF));
    auto it = g_resolved.find(fb & 0x1FFFFFFF);
    g_frameNumber++;
    if (it == g_resolved.end() || !w || !h) { SubmitFrame(false); g_cur ^= 1; return false; }
    Resolved& r = *it->second;
    BeginFrame();
    EndPass();
    VkCommandBuffer c = Cmd();
    Image& pi = g_present[g_cur];
    if (!pi.img || pi.w != w * S || pi.h != h * S) {
        if (pi.img) { vkDeviceWaitIdle(V->dev); pi.Destroy(); }
        CreateImage(pi, VK_FORMAT_R8G8B8A8_UNORM, w * S, h * S, false, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        ToGeneral(c, pi);
    }
    uint32_t swz = (fetch[3] >> 1) & 0xFFF;
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = g_frames[g_cur].dpool; dai.descriptorSetCount = 1; dai.pSetLayouts = &g_blitSetLayout;
    VkDescriptorSet set; if (vkAllocateDescriptorSets(V->dev, &dai, &set) != VK_SUCCESS) { SubmitFrame(false); g_cur ^= 1; return false; }
    VkDescriptorImageInfo ii{g_linearClamp, SwizzledView(r.im, swz, false, r.swapRB), VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wd{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii, nullptr, nullptr};
    vkUpdateDescriptorSets(V->dev, 1, &wd, 0, nullptr);
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; ca.imageView = pi.view; ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO}; ri.renderArea = {{0, 0}, {pi.w, pi.h}}; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    vkCmdBeginRendering(c, &ri);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, g_blitPipe);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, g_blitLayout, 0, 1, &set, 0, nullptr);
    VkViewport vp{0, 0, float(pi.w), float(pi.h), 0, 1}; VkRect2D sc{{0, 0}, {pi.w, pi.h}};
    vkCmdSetViewport(c, 0, 1, &vp); vkCmdSetScissor(c, 0, 1, &sc);
    float rect[4] = {0, 0, float(w * S) / r.im.w, float(h * S) / r.im.h};
    vkCmdPushConstants(c, g_blitLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, rect);
    vkCmdDraw(c, 3, 1, 0, 0);
    vkCmdEndRendering(c);
    FullBarrier(c);
    static const char* dumpDir = getenv("MADDEN_DUMP_FRAMES");
    static uint64_t dumpN = 0;
    bool dump = dumpDir && (++dumpN % 30 == 1);
    if (finishing) {
        fprintf(g_traceOut, "[trace] front buffer fetch swizzle %03X\n", swz);
        DumpImage(r.im, w * S, h * S, g_captureDir + "/front_buffer_raw.ppm");
        DumpImage(pi, pi.w, pi.h, g_captureDir + "/final.ppm");
        FinishCapture();
    }
    size_t dumpOff = 0;
    if (dump) {
        dumpOff = RingAlloc((size_t)pi.w * pi.h * 4, 256);
        VkBufferImageCopy bc{}; bc.bufferOffset = dumpOff; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bc.imageExtent = {pi.w, pi.h, 1};
        vkCmdCopyImageToBuffer(Cmd(), pi.img, VK_IMAGE_LAYOUT_GENERAL, g_frames[g_cur].ring.buf, 1, &bc);
    }
    SubmitFrame(true);
    if (dump) {
        char path[512]; snprintf(path, sizeof path, "%s/frame_%05llu.ppm", dumpDir, (unsigned long long)dumpN);
        if (FILE* f = fopen(path, "wb")) {
            fprintf(f, "P6\n%u %u\n255\n", pi.w, pi.h);
            const uint8_t* px = g_frames[g_cur].ring.ptr + dumpOff;
            std::vector<uint8_t> row(pi.w * 3);
            for (uint32_t y = 0; y < pi.h; y++) { for (uint32_t x = 0; x < pi.w; x++) memcpy(&row[x * 3], px + ((size_t)y * pi.w + x) * 4, 3); fwrite(row.data(), 1, row.size(), f); }
            fclose(f);
        }
    }
    PresenterSubmitRendered(pi.img, pi.w, pi.h);
    g_cur ^= 1;
    static uint64_t n = 0;
    if (++n <= 3 || n % 60 == 0) fprintf(stderr, "[render] frame %llu: %llu draws so far (%llu skipped), %zu shaders, %zu pipelines, %zu render targets, %zu textures\n",
                                          (unsigned long long)n, (unsigned long long)g_drawCount, (unsigned long long)g_skipped, g_shaders.size(), g_pipelines.size(), g_rts.size(), g_textures.size());
    return true;
}

}  // namespace

// ---------------------------------------------------------------- entry points (command processor thread)
uint64_t g_activeVs = 0, g_activePs = 0;
void RendererSetShader(uint32_t type, uint64_t hash, const std::vector<uint32_t>& dwords) {
    if (!g_microcode.count(hash)) g_microcode[hash] = dwords;
    if (type & 1) g_activePs = hash; else g_activeVs = hash;
}
bool RendererActive() {
    if (!g_tried) {
        if (g_settings.renderer == "none") { g_tried = true; return false; }
        g_active = Init();
    }
    return g_active;
}
void RendererDraw(const GpuDrawCall& call) {
    if (!RendererActive()) return;
    uint32_t edramMode = Reg(R_MODECONTROL) & 7;
    if (edramMode == 6) { Resolve(); return; }
    Draw(call, g_activeVs, g_activePs);
}
bool RendererSwap(uint32_t frontBufferPhys, uint32_t width, uint32_t height) {
    if (!RendererActive()) return false;
    uint32_t fetch[6]; for (int i = 0; i < 6; i++) fetch[i] = Reg(R_CONST_FETCH + i);
    return Swap(frontBufferPhys, width, height, fetch);
}
