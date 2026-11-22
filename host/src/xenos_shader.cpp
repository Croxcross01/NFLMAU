// Xenos shader microcode -> GLSL. See xenos_shader.h for credits and the shader interface.
#include "xenos_shader.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

namespace xenos {
namespace {

// ---------------------------------------------------------------- control flow
enum CfOp { kNop = 0, kExec, kExecEnd, kCondExec, kCondExecEnd, kCondExecPred, kCondExecPredEnd, kLoopStart, kLoopEnd,
            kCondCall, kReturn, kCondJmp, kAlloc, kCondExecPredClean, kCondExecPredCleanEnd, kMarkVsFetchDone };

struct Cf {   // one 48-bit control flow instruction
    uint64_t v;
    uint32_t op() const { return uint32_t(v >> 44) & 0xF; }
    uint32_t bits(int lo, int n) const { return uint32_t(v >> lo) & ((1u << n) - 1); }
    // exec family
    uint32_t execAddress() const { return bits(0, 12); }
    uint32_t execCount() const { return bits(12, 3); }
    uint32_t sequence() const { return bits(16, 12); }
    uint32_t boolAddress() const { return bits(34, 8); }
    bool condition() const { return bits(42, 1); }
    // loops / calls / jumps
    uint32_t address13() const { return bits(0, 13); }
    uint32_t loopId() const { return bits(16, 5); }
    bool predicatedBreak() const { return bits(21, 1); }
    bool unconditional() const { return bits(13, 1); }
    bool predicated() const { return bits(14, 1); }
};

const char* kComp = "xyzw";

struct Writer {
    std::ostringstream s;
    int indent = 1;
    void line(const std::string& t) { for (int i = 0; i < indent; i++) s << "    "; s << t << "\n"; }
};

std::string F(float v) { char b[64]; snprintf(b, sizeof b, "%.9g", v); std::string r = b; if (r.find_first_of(".en") == std::string::npos) r += ".0"; return r; }
std::string I(long v) { return std::to_string(v); }

class Translator {
public:
    Translator(ShaderStage st, const uint32_t* d, size_t n) : stage_(st), dw_(d, d + n) {}
    TranslatedShader Run();

private:
    ShaderStage stage_;
    std::vector<uint32_t> dw_;
    std::vector<Cf> cfs_;
    TranslatedShader out_;
    Writer body_;
    std::map<uint32_t, uint32_t> vfetchSlot_;          // constIndex -> rebase slot
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> texSlot_;
    bool vs() const { return stage_ == ShaderStage::kVertex; }

    std::string Fail(const std::string& e) { if (out_.error.empty()) out_.error = e; return ""; }
    void ParseCf();
    bool NeedsPcLoop() const;
    void EmitExec(const Cf& cf);
    void EmitAlu(const uint32_t* w);
    void EmitVFetch(const uint32_t* w);
    void EmitTFetch(const uint32_t* w);
    std::string AluSource(const uint32_t* w, int i, int ncomp);   // i = 1..3
    std::string ConstRef(const uint32_t* w, uint32_t reg, bool addressed) const;
    // vertex fetch: remembered for mini-fetches
    uint32_t lastVfetchConst_ = 0, lastVfetchStride_ = 0; std::string lastVfetchIndex_ = "0u";
};

void Translator::ParseCf() {
    // Control flow occupies the start of the program: 3 dwords hold 2 instructions. The first exec's address bounds it.
    size_t limit = dw_.size() / 3;   // in 3-dword groups
    for (size_t g = 0; g < limit; g++) {
        uint32_t a = dw_[g * 3], b = dw_[g * 3 + 1], c = dw_[g * 3 + 2];
        Cf c0{uint64_t(a) | (uint64_t(b & 0xFFFF) << 32)};
        Cf c1{uint64_t(b >> 16) | (uint64_t(c) << 16)};
        for (Cf cf : {c0, c1}) {
            cfs_.push_back(cf);
            uint32_t op = cf.op();
            if ((op == kExec || op == kExecEnd || (op >= kCondExec && op <= kCondExecPredEnd) || op == kCondExecPredClean ||
                 op == kCondExecPredCleanEnd) && cf.execCount())
                limit = std::min<size_t>(limit, cf.execAddress());
        }
    }
}

bool Translator::NeedsPcLoop() const {
    for (const Cf& cf : cfs_) { uint32_t op = cf.op(); if (op == kLoopStart || op == kLoopEnd || op == kCondCall || op == kReturn || op == kCondJmp) return true; }
    return false;
}

static bool IsEnd(uint32_t op) { return op == kExecEnd || op == kCondExecEnd || op == kCondExecPredEnd || op == kCondExecPredCleanEnd; }

std::string Translator::ConstRef(const uint32_t* w, uint32_t reg, bool addressed) const {
    std::string idx = I(reg + (vs() ? 0 : 256));
    if (addressed) idx += ((w[1] >> 29) & 1) ? " + a0" : " + aL";   // const_address_register_relative
    return "X.c[" + idx + "]";
}

// ALU source operand i (1..3) as a GLSL vec4 (ncomp == 4) or float expressions for scalar ops.
std::string Translator::AluSource(const uint32_t* w, int i, int ncomp) {
    uint32_t reg = (w[0] >> (8 * (3 - i))) & 0xFF;           // src1 bits 16-23, src2 8-15, src3 0-7
    bool temp = (w[0] >> (32 - i)) & 1;                       // src1_sel bit 31, src2 30, src3 29
    uint32_t swz = (w[1] >> (8 * (3 - i))) & 0xFF;            // src1_swiz 16-23 ...
    bool neg = (w[1] >> (24 + (3 - i))) & 1;                  // src3 neg bit 24, src2 25, src1 26
    std::string base;
    bool absv;
    if (temp) {
        std::string idx = I(reg & 0x3F); if (reg & 0x40) idx += " + aL";
        base = "r[" + idx + "]"; absv = reg & 0x80;
    } else {
        bool src1Temp = (w[0] >> 31) & 1, src2Temp = (w[0] >> 30) & 1;
        bool c0 = (w[1] >> 31) & 1, c1 = (w[1] >> 30) & 1;
        bool addressed = i == 1 ? c0 : i == 2 ? (src1Temp ? c0 : c1) : ((src1Temp && src2Temp) ? c0 : c1);
        base = ConstRef(w, reg, addressed); absv = (w[2] >> 7) & 1;   // abs_constants
    }
    if (absv) base = "abs(" + base + ")";
    auto comp = [&](uint32_t c) { return kComp[((swz >> (2 * c)) + c) & 3]; };
    std::string e;
    if (ncomp == 4) { e = base + "."; for (uint32_t c = 0; c < 4; c++) e += comp(c); }
    else if (ncomp == 1) e = base + "." + comp(3);
    else e = "vec2(" + base + "." + comp(3) + ", " + base + "." + comp(0) + ")";
    if (neg) e = "(-" + e + ")";
    return e;
}

void Translator::EmitAlu(const uint32_t* w) {
    // dword 0: src3_reg 0-7, src2_reg 8-15, src1_reg 16-23, vector_opc 24-28, src3_sel 29, src2_sel 30, src1_sel 31
    // dword 1: src3_swiz 0-7, src2_swiz 8-15, src1_swiz 16-23, negates 24-26, pred_condition 27, is_predicated 28,
    //          const_address_register_relative 29, const_1_rel_abs 30, const_0_rel_abs 31
    // dword 2: vector_dest 0-5, vector_dest_rel 6, abs_constants 7, scalar_dest 8-13, scalar_dest_rel 14, export 15,
    //          vector_write_mask 16-19, scalar_write_mask 20-23, vector_clamp 24, scalar_clamp 25, scalar_opc 26-31
    uint32_t vop = (w[0] >> 24) & 0x1F, sop = w[2] >> 26;
    uint32_t vdest = w[2] & 0x3F, sdest = (w[2] >> 8) & 0x3F;
    bool vdestRel = (w[2] >> 6) & 1, sdestRel = (w[2] >> 14) & 1, isExport = (w[2] >> 15) & 1;
    uint32_t vmask = (w[2] >> 16) & 0xF, smask = (w[2] >> 20) & 0xF;
    bool vclamp = (w[2] >> 24) & 1, sclamp = (w[2] >> 25) & 1;
    bool pred = (w[1] >> 28) & 1, predCond = (w[1] >> 27) & 1;

    uint32_t c0mask = 0, c1mask = 0, vres = vmask, sres = smask;
    if (isExport) { vres = vmask & ~smask; sres = smask & ~vmask; c1mask = vmask & smask; if (sdestRel) c0mask = 0xF & ~(vmask | smask); }

    Writer& o = body_;
    if (pred) { o.line(std::string("if (") + (predCond ? "p" : "!p") + ") {"); o.indent++; }
    o.line("{");
    o.indent++;
    // ---- vector op (always evaluated if it writes anything or has side effects)
    static const int vOperands[32] = {2,2,2,2,2,2,2,2,1,1,1,3,3,3,3,2,2,3,2,1,2,2,2,2,2,2,2,2,2,2,0,0};
    bool vecSideEffect = (vop >= 20 && vop <= 29);
    bool needVec = vres || vecSideEffect;
    if (needVec) {
        int n = vOperands[vop];
        std::string a = n >= 1 ? AluSource(w, 1, 4) : "vec4(0.0)", b = n >= 2 ? AluSource(w, 2, 4) : "vec4(0.0)", c = n >= 3 ? AluSource(w, 3, 4) : "vec4(0.0)";
        o.line("vec4 va = " + a + ", vb = " + b + ", vc = " + c + ";");
        std::string e;
        switch (vop) {
        case 0: e = "va + vb"; break;
        case 1: e = "va * vb"; break;
        case 2: e = "max(va, vb)"; break;
        case 3: e = "min(va, vb)"; break;
        case 4: e = "vec4(equal(va, vb))"; break;
        case 5: e = "vec4(greaterThan(va, vb))"; break;
        case 6: e = "vec4(greaterThanEqual(va, vb))"; break;
        case 7: e = "vec4(notEqual(va, vb))"; break;
        case 8: e = "fract(va)"; break;
        case 9: e = "trunc(va)"; break;
        case 10: e = "floor(va)"; break;
        case 11: e = "va * vb + vc"; break;
        case 12: e = "mix(vc, vb, vec4(equal(va, vec4(0.0))))"; break;
        case 13: e = "mix(vc, vb, vec4(greaterThanEqual(va, vec4(0.0))))"; break;
        case 14: e = "mix(vc, vb, vec4(greaterThan(va, vec4(0.0))))"; break;
        case 15: e = "vec4(dot(va, vb))"; break;
        case 16: e = "vec4(dot(va.xyz, vb.xyz))"; break;
        case 17: e = "vec4(dot(va.xy, vb.xy) + vc.x)"; break;
        case 18: e = "xeCube(va)"; break;
        case 19: e = "vec4(max(max(va.x, va.y), max(va.z, va.w)))"; break;
        case 20: case 21: case 22: case 23: {
            const char* opn = vop == 20 ? "==" : vop == 21 ? "!=" : vop == 22 ? ">" : ">=";
            o.line(std::string("p = (va.w == 0.0) && (vb.w ") + opn + " 0.0);");
            e = std::string("vec4((va.x == 0.0 && vb.x ") + opn + " 0.0) ? 0.0 : va.x + 1.0)"; break; }
        case 24: case 25: case 26: case 27: {
            const char* fn = vop == 24 ? "equal" : vop == 25 ? "greaterThan" : vop == 26 ? "greaterThanEqual" : "notEqual";
            out_.usesKill = true;
            o.line(std::string("bool vk = any(") + fn + "(va, vb));");
            if (!vs()) o.line("if (vk) discard;");
            e = "vec4(vk ? 1.0 : 0.0)"; break; }
        case 28: e = "vec4(1.0, va.y * vb.y, va.z, vb.w)"; break;
        case 29: o.line("a0 = clamp(int(floor(va.w + 0.5)), -256, 255);"); e = "max(va, vb)"; break;
        default: return void(Fail("vector opcode " + I(vop)));
        }
        o.line("vec4 vr = " + e + ";");
        if (vclamp) o.line("vr = clamp(vr, 0.0, 1.0);");
    }
    // ---- scalar op
    static const int sOperands[64] = {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,1,1,1,1,1,1,1,0,2,2,2,2,2,2,1,1,0};
    static const bool sTwo[64] = {1,0,1,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0};
    bool scalarSideEffect = (sop >= 23 && sop <= 39);
    bool needScalar = true;   // ps must always track the scalar result
    if (needScalar) {
        if (sop > 50 || sop == 41) return void(Fail("scalar opcode " + I(sop)));
        std::string a = "0.0", b = "0.0";
        if (sOperands[sop] == 1) {
            if (sTwo[sop]) { o.line("vec2 sab = " + AluSource(w, 3, 2) + ";"); a = "sab.x"; b = "sab.y"; }
            else { o.line("float sa1 = " + AluSource(w, 3, 1) + ";"); a = "sa1"; }
        } else if (sOperands[sop] == 2) {
            uint32_t src3 = w[0] & 0xFF, swz = w[1] & 0xFF; bool neg = (w[1] >> 24) & 1, absC = (w[2] >> 7) & 1;
            bool src1Temp = (w[0] >> 31) & 1, src2Temp = (w[0] >> 30) & 1;
            bool addressed = (src1Temp && src2Temp) ? ((w[1] >> 31) & 1) : ((w[1] >> 30) & 1);
            std::string cr = ConstRef(w, src3, addressed);
            uint32_t tempReg = (sop & 1) | (((w[0] >> 29) & 1) << 1) | (swz & 0x3C);
            std::string ca = cr + "." + kComp[((swz >> 6) + 3) & 3], tb = "r[" + I(tempReg) + "]." + kComp[swz & 3];
            if (absC) { ca = "abs(" + ca + ")"; tb = "abs(" + tb + ")"; }
            if (neg) { ca = "(-" + ca + ")"; tb = "(-" + tb + ")"; }
            o.line("float sa1 = " + ca + ", sb1 = " + tb + ";"); a = "sa1"; b = "sb1";
        }
        std::string e;
        switch (sop) {
        case 0: e = a + " + " + b; break;
        case 1: e = a + " + ps"; break;
        case 2: e = a + " * " + b; break;
        case 3: e = a + " * ps"; break;
        case 4: e = "((ps == -3.402823466e38 || isinf(ps) || isnan(ps) || isinf(" + b + ") || isnan(" + b + ") || " + b + " <= 0.0) ? -3.402823466e38 : " + a + " * ps)"; break;
        case 5: e = "max(" + a + ", " + b + ")"; break;
        case 6: e = "min(" + a + ", " + b + ")"; break;
        case 7: e = "(" + a + " == 0.0 ? 1.0 : 0.0)"; break;
        case 8: e = "(" + a + " > 0.0 ? 1.0 : 0.0)"; break;
        case 9: e = "(" + a + " >= 0.0 ? 1.0 : 0.0)"; break;
        case 10: e = "(" + a + " != 0.0 ? 1.0 : 0.0)"; break;
        case 11: e = "fract(" + a + ")"; break;
        case 12: e = "trunc(" + a + ")"; break;
        case 13: e = "floor(" + a + ")"; break;
        case 14: e = "exp2(" + a + ")"; break;
        case 15: e = "xeClampInf(log2(" + a + "))"; break;
        case 16: e = "log2(" + a + ")"; break;
        case 17: e = "xeClampInf(1.0 / " + a + ")"; break;
        case 18: e = "xeZeroInf(1.0 / " + a + ")"; break;
        case 19: e = "(1.0 / " + a + ")"; break;
        case 20: e = "xeClampInf(inversesqrt(" + a + "))"; break;
        case 21: e = "xeZeroInf(inversesqrt(" + a + "))"; break;
        case 22: e = "inversesqrt(" + a + ")"; break;
        case 23: o.line("a0 = clamp(int(floor(" + a + " + 0.5)), -256, 255);"); e = "max(" + a + ", " + b + ")"; break;
        case 24: o.line("a0 = clamp(int(floor(" + a + ")), -256, 255);"); e = "max(" + a + ", " + b + ")"; break;
        case 25: e = a + " - " + b; break;
        case 26: e = a + " - ps"; break;
        case 27: o.line("p = " + a + " == 0.0;"); e = "(p ? 0.0 : 1.0)"; break;
        case 28: o.line("p = " + a + " != 0.0;"); e = "(p ? 0.0 : 1.0)"; break;
        case 29: o.line("p = " + a + " > 0.0;"); e = "(p ? 0.0 : 1.0)"; break;
        case 30: o.line("p = " + a + " >= 0.0;"); e = "(p ? 0.0 : 1.0)"; break;
        case 31: o.line("p = " + a + " == 1.0;"); e = "(p ? 0.0 : (" + a + " == 0.0 ? 1.0 : " + a + "))"; break;
        case 32: o.line("float spm = " + a + " - 1.0; p = spm <= 0.0;"); e = "(p ? 0.0 : spm)"; break;
        case 33: o.line("p = false;"); e = "3.402823466e38"; break;
        case 34: o.line("p = " + a + " == 0.0;"); e = "(p ? 0.0 : " + a + ")"; break;
        case 35: case 36: case 37: case 38: case 39: {
            const char* cmp = sop == 35 ? " == 0.0" : sop == 36 ? " > 0.0" : sop == 37 ? " >= 0.0" : sop == 38 ? " != 0.0" : " == 1.0";
            out_.usesKill = true;
            o.line("bool sk = " + a + cmp + ";");
            if (!vs()) o.line("if (sk) discard;");
            e = "(sk ? 1.0 : 0.0)"; break; }
        case 40: e = "sqrt(" + a + ")"; break;
        case 42: case 43: e = a + " * " + b; break;
        case 44: case 45: e = a + " + " + b; break;
        case 46: case 47: e = a + " - " + b; break;
        case 48: e = "sin(" + a + ")"; break;
        case 49: e = "cos(" + a + ")"; break;
        case 50: e = "ps"; break;
        default: return void(Fail("scalar opcode " + I(sop)));
        }
        o.line("float sr = " + e + ";");
        if (sclamp) o.line("sr = clamp(sr, 0.0, 1.0);");
    }
    (void)scalarSideEffect;
    // ---- writes
    auto maskStr = [](uint32_t m) { std::string s; for (int c = 0; c < 4; c++) if (m & (1 << c)) s += kComp[c]; return s; };
    auto writeTo = [&](const std::string& dst, uint32_t vm, uint32_t sm, uint32_t z, uint32_t one) {
        for (int c = 0; c < 4; c++) {
            uint32_t bit = 1u << c;
            if (vm & bit) o.line(dst + "." + kComp[c] + " = vr." + kComp[c] + ";");
            else if (sm & bit) o.line(dst + "." + kComp[c] + " = sr;");
            else if (z & bit) o.line(dst + "." + kComp[c] + " = 0.0;");
            else if (one & bit) o.line(dst + "." + kComp[c] + " = 1.0;");
        }
    };
    if (isExport) {
        std::string dst;
        if (vs()) {
            if (vdest < 16) { dst = "xo[" + I(vdest) + "]"; out_.interpolatorsWritten |= 1u << vdest; }
            else if (vdest == 62) dst = "xpos";
            else if (vdest == 63) { dst = "xpsz"; out_.writesPointSize = true; }
            else if (vdest >= 32 && vdest <= 37) { out_.usesMemExport = true; }
            else return void(Fail("vs export " + I(vdest)));
        } else {
            if (vdest < 4) { dst = "xc[" + I(vdest) + "]"; out_.colorTargetsWritten |= 1u << vdest; }
            else if (vdest == 61) { dst = "xdepth"; out_.writesDepth = true; }
            else if (vdest >= 32 && vdest <= 37) { out_.usesMemExport = true; }
            else return void(Fail("ps export " + I(vdest)));
        }
        if (!dst.empty()) writeTo(dst, needVec ? vres : 0, sres, c0mask, c1mask);
    } else {
        if (vres && needVec) {
            std::string d = "r[" + I(vdest) + (vdestRel ? " + aL" : "") + "]";
            writeTo(d, vres, 0, 0, 0);
        }
        if (sres) {
            std::string d = "r[" + I(sdest) + (sdestRel ? " + aL" : "") + "]";
            writeTo(d, 0, sres, 0, 0);
        }
    }
    o.line("ps = sr;");
    o.indent--;
    o.line("}");
    if (pred) { o.indent--; o.line("}"); }
}

// Vertex fetch: decode one element from the per-draw vertex copy (big-endian guest data, endian swap from the constant).
void Translator::EmitVFetch(const uint32_t* w) {
    // dword0: op 0-4, src_reg 5-10, src_reg_am 11, dst_reg 12-17, dst_reg_am 18, must_be_one 19, const_index 20-24,
    //         const_index_sel 25-26, prefetch 27-29, src_swiz 30-31
    // dword1: dst_swiz 0-11, format_comp_all(signed) 12, num_format_all(int) 13, signed_rf_mode 14, is_index_rounded 15,
    //         format 16-21, exp_adjust 24-29, is_mini_fetch 30, is_predicated 31
    // dword2: stride 0-7, offset 8-30, pred_condition 31
    uint32_t srcReg = (w[0] >> 5) & 0x3F, dstReg = (w[0] >> 12) & 0x3F;
    bool srcRel = (w[0] >> 11) & 1, dstRel = (w[0] >> 18) & 1;
    uint32_t constIdx = ((w[0] >> 20) & 0x1F) * 3 + ((w[0] >> 25) & 3), srcSwz = w[0] >> 30;
    uint32_t dstSwz = w[1] & 0xFFF; bool isSigned = (w[1] >> 12) & 1, isInt = (w[1] >> 13) & 1, rfNoZero = (w[1] >> 14) & 1;
    bool rounded = (w[1] >> 15) & 1; uint32_t fmt = (w[1] >> 16) & 0x3F; int expAdj = int32_t(w[1] << 2) >> 26;
    bool mini = (w[1] >> 30) & 1, pred = (w[1] >> 31) & 1, predCond = w[2] >> 31;
    uint32_t stride = w[2] & 0xFF; int offset = int32_t(w[2] << 1) >> 9;
    Writer& o = body_;
    if (pred) { o.line(std::string("if (") + (predCond ? "p" : "!p") + ") {"); o.indent++; }
    o.line("{");
    o.indent++;
    std::string idx;
    if (mini) { constIdx = lastVfetchConst_; idx = lastVfetchIndex_; stride = lastVfetchStride_; }   // minis inherit index, constant and stride
    else {
        std::string s = "r[" + I(srcReg) + (srcRel ? " + aL" : "") + "]." + kComp[srcSwz];
        idx = "uint(max(int(" + std::string(rounded ? "floor(" + s + " + 0.5)" : "floor(" + s + ")") + "), 0))";
        lastVfetchConst_ = constIdx;
        lastVfetchStride_ = stride;
        if (out_.vfetch.empty()) { out_.firstFetchStride = stride; out_.firstFetchOffset = (uint32_t)offset; }
    }
    if (!vfetchSlot_.count(constIdx)) { uint32_t slot = (uint32_t)vfetchSlot_.size(); vfetchSlot_[constIdx] = slot; out_.vfetch.push_back({constIdx}); }
    uint32_t slot = vfetchSlot_[constIdx];
    o.line("uint vi = " + idx + ";");
    if (!mini) lastVfetchIndex_ = "vi";   // only valid within the same scope; recomputed below for safety
    o.line("uint vaddr = uint(X.vrebase[" + I(slot / 4) + "][" + I(slot % 4) + "]) + vi * " + I(stride) + "u + uint(" + I(offset) + ");");
    o.line("uint vend = X.fetch[" + I(constIdx / 2) + "][" + I((constIdx % 2) * 2 + 1) + "] & 3u;");
    auto word = [&](int k) { return "xeSwap(mem[vaddr + " + I(k) + "u], vend)"; };
    std::string comps[4] = {"0.0", "0.0", "0.0", "1.0"};
    int ncomp = 0;
    auto packed = [&](std::initializer_list<std::pair<int, int>> fields) {   // (offset, width) per component in word 0
        o.line("uint vw0 = " + word(0) + ";");
        int c = 0;
        for (auto [off, wd] : fields) {
            std::string raw = "float(bitfieldExtract(vw0, " + I(off) + ", " + I(wd) + "))";
            std::string sraw = "float(bitfieldExtract(int(vw0), " + I(off) + ", " + I(wd) + "))";
            float maxu = float((1u << wd) - 1), maxs = float((1u << (wd - 1)) - 1);
            std::string v;
            if (isSigned) {
                if (isInt) v = sraw;
                else if (rfNoZero) v = "((" + sraw + " * 2.0 + 1.0) / " + F(maxu) + ")";
                else v = "max(" + sraw + " / " + F(maxs) + ", -1.0)";
            } else v = isInt ? raw : "(" + raw + " / " + F(maxu) + ")";
            comps[c++] = v;
        }
        ncomp = c;
    };
    switch (fmt) {
    case 6: packed({{0, 8}, {8, 8}, {16, 8}, {24, 8}}); break;          // 8_8_8_8
    case 7: packed({{0, 10}, {10, 10}, {20, 10}, {30, 2}}); break;      // 2_10_10_10
    case 16: packed({{0, 11}, {11, 11}, {22, 10}}); break;              // 10_11_11
    case 17: packed({{0, 10}, {10, 11}, {21, 11}}); break;              // 11_11_10
    case 25: packed({{0, 16}, {16, 16}}); break;                        // 16_16
    case 26: {                                                          // 16_16_16_16
        o.line("uint vw0 = " + word(0) + ", vw1 = " + word(1) + ";");
        for (int c = 0; c < 4; c++) {
            std::string wv = c < 2 ? "vw0" : "vw1"; int off = (c & 1) * 16;
            std::string raw = "float(bitfieldExtract(" + wv + ", " + I(off) + ", 16))", sraw = "float(bitfieldExtract(int(" + wv + "), " + I(off) + ", 16))";
            comps[c] = isSigned ? (isInt ? sraw : rfNoZero ? "((" + sraw + " * 2.0 + 1.0) / 65535.0)" : "max(" + sraw + " / 32767.0, -1.0)") : (isInt ? raw : "(" + raw + " / 65535.0)");
        }
        ncomp = 4; break; }
    case 31: o.line("vec2 vh0 = unpackHalf2x16(" + word(0) + ");"); comps[0] = "vh0.x"; comps[1] = "vh0.y"; ncomp = 2; break;
    case 32: o.line("vec2 vh0 = unpackHalf2x16(" + word(0) + "), vh1 = unpackHalf2x16(" + word(1) + ");");
             comps[0] = "vh0.x"; comps[1] = "vh0.y"; comps[2] = "vh1.x"; comps[3] = "vh1.y"; ncomp = 4; break;
    case 33: case 34: case 35: {                                         // 32 / 32_32 / 32_32_32_32 integer
        int n = fmt - 32;
        for (int c = 0; c < n; c++) {
            std::string wv = word(c);
            comps[c] = isSigned ? "float(int(" + wv + "))" : "float(" + wv + ")";
            if (!isInt) comps[c] = "(" + comps[c] + (isSigned ? " / 2147483647.0)" : " / 4294967295.0)");
        }
        ncomp = n; break; }
    case 36: case 37: case 38: case 57: {                                 // 32 float variants
        int n = fmt == 36 ? 1 : fmt == 37 ? 2 : fmt == 38 ? 4 : 3;
        for (int c = 0; c < n; c++) comps[c] = "uintBitsToFloat(" + word(c) + ")";
        ncomp = n; break; }
    default: Fail("vertex format " + I(fmt)); break;
    }
    (void)ncomp;
    if (expAdj) for (int c = 0; c < 4; c++) if (c < ncomp) comps[c] = "(" + comps[c] + " * " + F(float(std::ldexp(1.0, expAdj))) + ")";
    o.line("vec4 vf = vec4(" + comps[0] + ", " + comps[1] + ", " + comps[2] + ", " + comps[3] + ");");
    std::string d = "r[" + I(dstReg) + (dstRel ? " + aL" : "") + "]";
    for (int c = 0; c < 4; c++) {
        uint32_t s = (dstSwz >> (3 * c)) & 7;
        if (s == 7) continue;
        o.line(d + "." + kComp[c] + " = " + (s < 4 ? std::string("vf.") + kComp[s] : s == 4 ? "0.0" : "1.0") + ";");
    }
    o.indent--;
    o.line("}");
    if (!mini) {
        // remember the index expression for later mini fetches in a function-scope variable
        o.line("xvi = " + idx + ";");
        lastVfetchIndex_ = "xvi";
    }
    if (pred) { o.indent--; o.line("}"); }
}

void Translator::EmitTFetch(const uint32_t* w) {
    uint32_t op = w[0] & 0x1F;
    uint32_t srcReg = (w[0] >> 5) & 0x3F, dstReg = (w[0] >> 12) & 0x3F;
    bool srcRel = (w[0] >> 11) & 1, dstRel = (w[0] >> 18) & 1;
    uint32_t constIdx = (w[0] >> 20) & 0x1F; bool denorm = (w[0] >> 25) & 1; uint32_t srcSwz = (w[0] >> 26) & 0x3F;
    uint32_t dstSwz = w[1] & 0xFFF; bool useRegLod = (w[1] >> 29) & 1, pred = (w[1] >> 31) & 1, predCond = w[2] >> 31;
    int lodBias = int32_t(w[2] << 23) >> 25; uint32_t dim = (w[2] >> 14) & 3;
    int offX = int32_t(w[2] << 11) >> 27, offY = int32_t(w[2] << 6) >> 27;
    Writer& o = body_;
    if (pred) { o.line(std::string("if (") + (predCond ? "p" : "!p") + ") {"); o.indent++; }
    std::string src = "r[" + I(srcReg) + (srcRel ? " + aL" : "") + "]";
    std::string coord = src + "." + kComp[srcSwz & 3] + kComp[(srcSwz >> 2) & 3] + kComp[(srcSwz >> 4) & 3];
    std::string d = "r[" + I(dstReg) + (dstRel ? " + aL" : "") + "]";
    if (op == 24) {   // setTexLOD
        o.line("xlod = " + src + "." + kComp[srcSwz & 3] + ";");
    } else if (op != 1) {   // gradients / weights / border queries: rarely used, return zeros
        o.line("{ vec4 tf = vec4(0.0);");
        for (int c = 0; c < 4; c++) { uint32_t s = (dstSwz >> (3 * c)) & 7; if (s != 7) o.line("  " + d + "." + kComp[c] + " = " + (s < 4 ? std::string("tf.") + kComp[s] : s == 4 ? "0.0" : "1.0") + ";"); }
        o.line("}");
    } else {
        auto key = std::make_pair(constIdx, dim);
        if (!texSlot_.count(key)) { uint32_t s = (uint32_t)texSlot_.size(); texSlot_[key] = s; out_.textures.push_back({constIdx, dim}); }
        uint32_t slot = texSlot_[key];
        std::string sampler = std::string(vs() ? "xvt" : "xt") + I(slot);
        o.line("{");
        o.indent++;
        o.line("vec3 tc = " + coord + ";");
        float bias = lodBias / 16.0f;
        // LOD (Xenia's translator): with use_computed_lod (pixel shaders) the GPU picks the level from derivatives and
        // the register LOD (setTexLOD) is a bias on top of it; without it the register value is the absolute level.
        // An instruction mip filter of "base map" samples level 0 only.
        bool useCompLod = (w[1] >> 28) & 1, baseMap = ((w[1] >> 16) & 3) == 2;
        bool explicitLod = vs() || baseMap || !useCompLod;
        std::string lodAbs = baseMap ? std::string("0.0") : (useRegLod ? "xlod + " : "") + F(bias);
        std::string lodBiasExpr = (useRegLod ? "xlod + " : "") + F(bias);
        auto sample = [&](const std::string& smp, const std::string& c) {
            if (explicitLod) return "textureLod(" + smp + ", " + c + ", " + lodAbs + ")";
            if (!useRegLod && bias == 0) return "texture(" + smp + ", " + c + ")";
            return "texture(" + smp + ", " + c + ", " + lodBiasExpr + ")";
        };
        if (dim <= 1) {
            // guest texture size from the fetch constant (host images can be scaled by the internal resolution)
            uint32_t d2 = constIdx * 6 + 2;
            o.line("uint tsd = X.fetch[" + I(d2 / 4) + "][" + I(d2 % 4) + "];");
            o.line("vec2 tsz = vec2(float((tsd & 0x1FFFu) + 1u), float(((tsd >> 13) & 0x1FFFu) + 1u));");
            if (denorm) o.line("tc.xy /= tsz;");
            if (offX || offY) o.line("tc.xy += vec2(" + F(offX * 0.5f) + ", " + F(offY * 0.5f) + ") / tsz;");
            if (dim == 0) o.line("tc.y = 0.0;");
            o.line("vec4 tf = " + sample(sampler, "tc.xy") + ";");
        } else if (dim == 2) {
            // 3D data -> sampler3D; stacked (or plain 2D) data -> 2D array with layer = z * depth (as Xenia)
            uint32_t d1 = constIdx * 6 + 1, d2 = constIdx * 6 + 2, d5 = constIdx * 6 + 5;
            std::string asampler = std::string(vs() ? "xvta" : "xta") + I(slot);
            o.line("uint tfd1 = X.fetch[" + I(d1 / 4) + "][" + I(d1 % 4) + "], tfd2 = X.fetch[" + I(d2 / 4) + "][" + I(d2 % 4) + "], tfd5 = X.fetch[" + I(d5 / 4) + "][" + I(d5 % 4) + "];");
            o.line("bool tvol = ((tfd5 >> 9) & 3u) == 2u;   // fetch constant dimension k3D (stacked arrays are stored as 2D)");
            o.line("vec4 tf;");
            o.line("if (tvol) {");
            if (denorm) o.line("    tc /= vec3(float((tfd2 & 0x7FFu) + 1u), float(((tfd2 >> 11) & 0x7FFu) + 1u), float(((tfd2 >> 22) & 0x3FFu) + 1u));");
            o.line("    tf = " + sample(sampler, "tc") + ";");
            o.line("} else {");
            o.line("    float tlayers = ((tfd1 >> 10) & 1u) != 0u ? float(((tfd2 >> 26) & 0x3Fu) + 1u) : 1.0;");
            if (denorm) o.line("    vec3 ta = vec3(tc.xy / vec2(float((tfd2 & 0x1FFFu) + 1u), float(((tfd2 >> 13) & 0x1FFFu) + 1u)), clamp(floor(tc.z), 0.0, tlayers - 1.0));");
            else o.line("    vec3 ta = vec3(tc.xy, clamp(floor(tc.z * tlayers), 0.0, tlayers - 1.0));");
            o.line("    tf = " + sample(asampler, "ta") + ";");
            o.line("}");
        } else {
            // cube: tc = (s, t, face) after the cube instruction; s,t in [1, 2] (scaled by 1/|ma| and offset by 1.5)
            o.line("vec3 cdir = xeCubeDir(tc);");
            o.line("vec4 tf = " + sample(sampler, "cdir") + ";");
        }
        {   // fetch constant exp_adjust (dword 3 bits 13-18): result scaled by 2^exp_adjust
            uint32_t d3 = constIdx * 6 + 3;
            o.line("{ int tea = int(X.fetch[" + I(d3 / 4) + "][" + I(d3 % 4) + "] << 13) >> 26; if (tea != 0) tf *= exp2(float(tea)); }");
        }
        for (int c = 0; c < 4; c++) {
            uint32_t s = (dstSwz >> (3 * c)) & 7;
            if (s == 7) continue;
            o.line(d + "." + kComp[c] + " = " + (s < 4 ? std::string("tf.") + kComp[s] : s == 4 ? "0.0" : "1.0") + ";");
        }
        o.indent--;
        o.line("}");
    }
    if (pred) { o.indent--; o.line("}"); }
}

void Translator::EmitExec(const Cf& cf) {
    uint32_t addr = cf.execAddress(), count = cf.execCount(), seq = cf.sequence();
    for (uint32_t i = 0; i < count; i++) {
        size_t at = size_t(addr + i) * 3;
        if (at + 3 > dw_.size()) { Fail("exec past end"); return; }
        const uint32_t* w = &dw_[at];
        bool fetch = (seq >> (i * 2)) & 1;
        if (fetch) { if ((w[0] & 0x1F) == 0) EmitVFetch(w); else EmitTFetch(w); }
        else { uint32_t a[3] = {w[2], w[1], w[0]}; EmitAlu(a); }   // ALU: dword 0 holds dests, dword 2 sources
    }
}

TranslatedShader Translator::Run() {
    out_.stage = stage_;
    ParseCf();
    bool pcLoop = NeedsPcLoop();
    Writer& o = body_;
    // locate the end: first unconditional end after which nothing is reachable (for straight-line shaders)
    if (pcLoop) {
        o.line("int pc = 0;");
        o.line("int lc[4] = int[4](0, 0, 0, 0); int ls[4] = int[4](0, 0, 0, 0); int lsp = 0; int cs[4] = int[4](0, 0, 0, 0); int csp = 0;");
        o.line("for (int guard = 0; guard < 65536; guard++) {");
        o.indent++;
        o.line("switch (pc) {");
    }
    for (size_t i = 0; i < cfs_.size(); i++) {
        const Cf& cf = cfs_[i];
        uint32_t op = cf.op();
        if (pcLoop) { o.line("case " + I(i) + ":"); }
        std::string cond;
        auto boolTest = [&](uint32_t ba, bool c) {
            return std::string("(((X.b[") + I(ba / 128) + "][" + I((ba / 32) % 4) + "] >> " + I(ba % 32) + "u) & 1u) " + (c ? "!=" : "==") + " 0u)";
        };
        switch (op) {
        case kNop: case kAlloc: case kMarkVsFetchDone: break;
        case kExec: case kExecEnd:
            EmitExec(cf); break;
        case kCondExec: case kCondExecEnd: case kCondExecPredClean: case kCondExecPredCleanEnd:
            o.line("if " + boolTest(cf.boolAddress(), cf.condition()) + " {"); o.indent++; EmitExec(cf); o.indent--; o.line("}"); break;
        case kCondExecPred: case kCondExecPredEnd:
            o.line(std::string("if (") + (cf.condition() ? "p" : "!p") + ") {"); o.indent++; EmitExec(cf); o.indent--; o.line("}"); break;
        case kLoopStart: {
            uint32_t id = cf.loopId();
            o.line("{ uint lcst = X.loops[" + I(id / 4) + "][" + I(id % 4) + "];");
            o.line("  lsp++; lc[lsp & 3] = int(lcst & 0xFFu); ls[lsp & 3] = aL; aL = int((lcst >> 8) & 0xFFu);");
            o.line("  if (lc[lsp & 3] <= 0) { aL = ls[lsp & 3]; lsp--; pc = " + I(cf.address13()) + "; continue; } }");
            break; }
        case kLoopEnd: {
            uint32_t id = cf.loopId();
            o.line("{ uint lcst = X.loops[" + I(id / 4) + "][" + I(id % 4) + "];");
            std::string brk = cf.predicatedBreak() ? std::string(" && ") + (cf.condition() ? "!p" : "p") : "";
            o.line("  lc[lsp & 3]--; aL += int(int(lcst << 8) >> 24);");
            o.line("  if (lc[lsp & 3] > 0" + brk + ") { pc = " + I(cf.address13()) + "; continue; }");
            o.line("  aL = ls[lsp & 3]; lsp--; }");
            break; }
        case kCondCall: case kCondJmp: {
            std::string c = cf.unconditional() ? "true" : cf.predicated() ? std::string(cf.condition() ? "p" : "!p") : boolTest(cf.boolAddress(), cf.condition());
            if (op == kCondCall) o.line("if (" + c + ") { csp++; cs[csp & 3] = " + I(i + 1) + "; pc = " + I(cf.address13()) + "; continue; }");
            else o.line("if (" + c + ") { pc = " + I(cf.address13()) + "; continue; }");
            break; }
        case kReturn:
            o.line("{ pc = cs[csp & 3]; csp--; continue; }"); break;
        default: Fail("cf opcode " + I(op)); break;
        }
        if (IsEnd(op)) { o.line(pcLoop ? "pc = -1; break;" : "return;"); if (!pcLoop) break; }
    }
    if (pcLoop) {
        o.line("default: pc = -1; break;");
        o.line("}");
        o.line("if (pc < 0) break;");
        o.indent--;
        o.line("}");
    }

    // ---- assemble the shader
    std::ostringstream g;
    g << "#version 450\n" << CommonPrelude();
    if (vs()) {
        g << "layout(std430, set = 0, binding = 1) readonly buffer XeMem { uint mem[]; };\n";
        for (int i = 0; i < 16; i++) if (out_.interpolatorsWritten & (1u << i)) g << "layout(location = " << i << ") out vec4 xv" << i << ";\n";
        for (size_t i = 0; i < out_.textures.size(); i++) {
            const char* t = out_.textures[i].dimension == 2 ? "sampler3D" : out_.textures[i].dimension == 3 ? "samplerCube" : "sampler2D";
            g << "layout(set = 0, binding = " << 40 + i << ") uniform " << t << " xvt" << i << ";\n";
            if (out_.textures[i].dimension == 2) g << "layout(set = 0, binding = " << 48 + i << ") uniform sampler2DArray xvta" << i << ";\n";
        }
        g << "vec4 xo[16]; vec4 xpos = vec4(0.0, 0.0, 0.0, 1.0); vec4 xpsz = vec4(0.0);\n";
    } else {
        for (int i = 0; i < 16; i++) g << "layout(location = " << i << ") in vec4 xv" << i << ";\n";
        for (int i = 0; i < 4; i++) g << "layout(location = " << i << ") out vec4 xcolor" << i << ";\n";
        for (size_t i = 0; i < out_.textures.size(); i++) {
            const char* t = out_.textures[i].dimension == 2 ? "sampler3D" : out_.textures[i].dimension == 3 ? "samplerCube" : "sampler2D";
            g << "layout(set = 0, binding = " << 8 + i << ") uniform " << t << " xt" << i << ";\n";
            if (out_.textures[i].dimension == 2) g << "layout(set = 0, binding = " << 24 + i << ") uniform sampler2DArray xta" << i << ";\n";
        }
        g << "vec4 xc[4]; vec4 xdepth = vec4(0.0);\n";
        g << "void xeAlphaTest(float a) {\n"
             "    int f = X.misc.z; float ref = X.alphaRef.x; bool pass = true;\n"
             "    if (f == 0) pass = false; else if (f == 1) pass = a < ref; else if (f == 2) pass = a == ref; else if (f == 3) pass = a <= ref;\n"
             "    else if (f == 4) pass = a > ref; else if (f == 5) pass = a != ref; else if (f == 6) pass = a >= ref;\n"
             "    if (!pass) discard;\n}\n";
    }
    g << "vec4 r[64]; bool p = false; int a0 = 0; int aL = 0; float ps = 0.0; float xlod = 0.0; uint xvi = 0u;\n";
    g << "void xmain() {\n" << o.s.str() << "}\n";
    g << "void main() {\n";
    g << "    for (int i = 0; i < 64; i++) r[i] = vec4(0.0);\n";
    if (vs()) {
        g << "    for (int i = 0; i < 16; i++) xo[i] = vec4(0.0);\n";
        g << "    r[0].x = float(gl_VertexIndex);\n";
        g << "    xmain();\n";
        for (int i = 0; i < 16; i++) if (out_.interpolatorsWritten & (1u << i)) g << "    xv" << i << " = xo[" << i << "];\n";
        g << "    gl_Position = xeVsPosition(xpos);\n";
        g << (out_.writesPointSize ? "    gl_PointSize = xpsz.x > 0.0 ? xpsz.x : 1.0;\n" : "    gl_PointSize = 1.0;\n");
    } else {
        for (int i = 0; i < 16; i++) g << "    r[" << i << "] = xv" << i << ";\n";
        g << "    if (X.misc.y >= 0) r[X.misc.y] = vec4(gl_FragCoord.xy, gl_FrontFacing ? 0.0 : 1.0, 0.0);\n";
        g << "    for (int i = 0; i < 4; i++) xc[i] = vec4(0.0);\n";
        g << "    xmain();\n";
        g << "    xeAlphaTest(xc[0].a);\n";
        g << "    xcolor0 = xc[0] * X.expScale.x; xcolor1 = xc[1] * X.expScale.y; xcolor2 = xc[2] * X.expScale.z; xcolor3 = xc[3] * X.expScale.w;\n";
        if (out_.writesDepth) g << "    gl_FragDepth = clamp(xdepth.x, 0.0, 1.0);\n";
    }
    g << "}\n";
    out_.glsl = g.str();
    out_.ok = out_.error.empty();
    return out_;
}

}  // namespace

const char* CommonPrelude() {
    return R"(
layout(std140, set = 0, binding = 0) uniform XeConsts {
    vec4 c[512];
    uvec4 b[2];
    uvec4 loops[8];
    uvec4 fetch[48];
    vec4 ndcScale;
    vec4 ndcOffset;
    ivec4 misc;      // x PA_CL_VTE_CNTL, y PS param_gen register, z alpha test function (0-7, 7 = always), w unused
    vec4 alphaRef;   // x reference
    vec4 expScale;   // per render target: 2^RB_COLOR_INFO.color_exp_bias
    ivec4 vrebase[24];
} X;
uint xeSwap(uint v, uint e) {
    if (e == 1u) return ((v << 8) & 0xFF00FF00u) | ((v >> 8) & 0x00FF00FFu);
    if (e == 2u) return (v << 24) | ((v << 8) & 0x00FF0000u) | ((v >> 8) & 0x0000FF00u) | (v >> 24);
    if (e == 3u) return (v << 16) | (v >> 16);
    return v;
}
float xeClampInf(float v) { return isinf(v) ? (v < 0.0 ? -3.402823466e38 : 3.402823466e38) : v; }
float xeZeroInf(float v) { return isinf(v) ? 0.0 : v; }
vec4 xeCube(vec4 s) {
    vec3 d = vec3(s.z, s.w, s.x);   // operand is .z_xy
    vec3 a = abs(d);
    float ma, sc, tc, face;
    if (a.z >= a.x && a.z >= a.y) { ma = d.z; face = d.z < 0.0 ? 5.0 : 4.0; sc = d.z < 0.0 ? -d.x : d.x; tc = -d.y; }
    else if (a.y >= a.x) { ma = d.y; face = d.y < 0.0 ? 3.0 : 2.0; sc = d.x; tc = d.y < 0.0 ? -d.z : d.z; }
    else { ma = d.x; face = d.x < 0.0 ? 1.0 : 0.0; sc = d.x < 0.0 ? d.z : -d.z; tc = -d.y; }
    return vec4(tc, sc, 2.0 * ma, face);
}
vec3 xeCubeDir(vec3 t) {   // inverse of the cube ALU instruction (mapping as in Xenia's fetch translation)
    float c0 = t.x * 2.0 - 3.0, c1 = t.y * 2.0 - 3.0;
    int face = int(clamp(floor(t.z), 0.0, 5.0));
    bool neg = (face & 1) != 0; float sgn = neg ? -1.0 : 1.0;
    int axis = face >> 1;
    if (axis == 0) return vec3(sgn, -c1, neg ? c0 : -c0);
    if (axis == 1) return vec3(c0, sgn, neg ? -c1 : c1);
    return vec3(neg ? -c0 : c0, -c1, sgn);
}
vec4 xeVsPosition(vec4 p) {
    int vte = X.misc.x;
    if ((vte & (1 << 10)) == 0) p.w = 1.0 / p.w;           // vtx_w0_fmt clear: the shader exported 1/W0
    if ((vte & (1 << 8)) != 0) p.xy *= p.w;                // vtx_xy_fmt: xy already divided by W
    if ((vte & (1 << 9)) != 0) p.z *= p.w;                 // vtx_z_fmt
    p.xyz = p.xyz * X.ndcScale.xyz + X.ndcOffset.xyz * p.w;
    if (X.misc.w != 0) p.z = clamp(p.z, 0.0, abs(p.w));  // clipping disabled on the guest: never depth-clip
    return p;
}
)";
}

TranslatedShader Translate(ShaderStage stage, const uint32_t* dwords, size_t count) {
    Translator t(stage, dwords, count);
    return t.Run();
}

}  // namespace xenos
