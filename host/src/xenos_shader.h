// Xenos (Xbox 360 GPU) shader microcode -> GLSL translator.
// Microcode layouts and instruction semantics follow Xenia's ucode.h / shader translators
// (BSD-3-Clause, Copyright Ben Vanik and Xenia contributors); this is an independent, simplified implementation.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace xenos {

enum class ShaderStage { kVertex = 0, kPixel = 1 };

struct VertexFetchInfo {   // one distinct vertex fetch constant used by a vertex shader
    uint32_t constIndex;   // 0..95 (fetch constant slot = constIndex / 3, dword pair = constIndex % 3)
};
struct TextureFetchInfo {
    uint32_t constIndex;   // 0..31
    uint32_t dimension;    // 0 1D, 1 2D, 2 3D/stacked, 3 cube
};

struct TranslatedShader {
    ShaderStage stage;
    bool ok = false;
    std::string error;
    std::string glsl;
    std::vector<VertexFetchInfo> vfetch;     // distinct vertex fetch constants (binding order = vertex rebase slot)
    std::vector<TextureFetchInfo> textures;  // distinct (constIndex, dimension) pairs; binding = 8 + position
    bool usesKill = false, writesDepth = false, usesMemExport = false, writesPointSize = false;
    uint32_t colorTargetsWritten = 0;        // pixel shaders: bit per oC#
    uint32_t interpolatorsWritten = 0;       // vertex shaders: bit per o#
    uint32_t firstFetchStride = 0, firstFetchOffset = 0;   // first full vertex fetch (dwords), for CPU-side position reads
};

// Uniform/storage interface shared by every translated shader (set 0):
//   binding 0: uniform Xe { vec4 c[512]; uvec4 b[2]; uvec4 loops[8]; vec4 ndcScale; vec4 ndcOffset;
//                           ivec4 misc; ivec4 vrebase[24]; }
//              misc.x = PA_CL_VTE_CNTL, misc.y = pixel shader param_gen register (-1 none), misc.z = alpha test (func|ref)
//   binding 1: readonly buffer Mem { uint mem[]; }   (vertex data copied per draw; vrebase gives dword offsets)
//   binding 8+i: texture i of TranslatedShader::textures (pixel and vertex shaders use separate descriptor sets? no:
//              vertex shader textures use binding 40+i)
TranslatedShader Translate(ShaderStage stage, const uint32_t* dwords, size_t count);

// Shared GLSL prelude text (uniform block etc.), mostly for tools.
const char* CommonPrelude();

}  // namespace xenos
