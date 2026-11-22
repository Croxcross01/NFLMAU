#include <cstring>
#include <map>
#include <string>
// Translate captured Xenos shaders (vs_*.bin / ps_*.bin) to GLSL and compile them with glslang.
#include "../src/xenos_shader.h"
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>
int main(int argc, char** argv) {
    glslang_initialize_process();
    int ok = 0, bad = 0, trans = 0;
    std::map<std::string,int> errs;
    for (auto& e : std::filesystem::directory_iterator(argv[1])) {
        std::string n = e.path().filename().string();
        if (n.size() < 4 || n.substr(n.size() - 4) != ".bin") continue;
        bool ps = n[0] == 'p';
        std::ifstream f(e.path(), std::ios::binary); std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
        std::vector<uint32_t> dw(b.size() / 4); memcpy(dw.data(), b.data(), dw.size() * 4);
        // captured already in host order (ReadU32 swaps)
        auto t = xenos::Translate(ps ? xenos::ShaderStage::kPixel : xenos::ShaderStage::kVertex, dw.data(), dw.size());
        if (!t.ok) { trans++; errs[t.error]++; continue; }
        if (argc > 2 && n.find(argv[2]) != std::string::npos) printf("%s\n", t.glsl.c_str());
        glslang_input_t in{}; in.language = GLSLANG_SOURCE_GLSL; in.stage = ps ? GLSLANG_STAGE_FRAGMENT : GLSLANG_STAGE_VERTEX;
        in.client = GLSLANG_CLIENT_VULKAN; in.client_version = GLSLANG_TARGET_VULKAN_1_1; in.target_language = GLSLANG_TARGET_SPV;
        in.target_language_version = GLSLANG_TARGET_SPV_1_3; in.code = t.glsl.c_str(); in.default_version = 450;
        in.default_profile = GLSLANG_NO_PROFILE; in.messages = GLSLANG_MSG_DEFAULT_BIT; in.resource = glslang_default_resource();
        glslang_shader_t* sh = glslang_shader_create(&in);
        if (!glslang_shader_preprocess(sh, &in) || !glslang_shader_parse(sh, &in)) {
            bad++; if (bad <= 3) printf("== %s\n%s\n", n.c_str(), glslang_shader_get_info_log(sh));
        } else ok++;
        glslang_shader_delete(sh);
    }
    printf("compiled %d, glsl errors %d, translation failures %d\n", ok, bad, trans);
    for (auto& [k, v] : errs) printf("  %s x%d\n", k.c_str(), v);
}
