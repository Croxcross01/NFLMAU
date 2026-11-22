// Video-mode queries and the first GPU-init calls. Rendering itself is not implemented yet: this only answers
// the questions the game asks during startup (1280x720, widescreen, 60 Hz).
#include "kernel.h"

#define IMP(n) PPC_FUNC(__imp__##n)
#define TRACE(n) HostTrace("__imp__" #n, ctx)

// X_VIDEO_MODE: width, height, interlaced, widescreen, hidef, refresh (float), standard, 0x4A, 0x01, reserved[3]
static void FillVideoMode(uint32_t p) {
    memset(g_base + p, 0, 0x30);
    WriteU32(p + 0x00, 1280); WriteU32(p + 0x04, 720);
    WriteU32(p + 0x08, 0); WriteU32(p + 0x0C, 1); WriteU32(p + 0x10, 1);
    float hz = 60.0f; uint32_t bits; memcpy(&bits, &hz, 4); WriteU32(p + 0x14, bits);
    WriteU32(p + 0x18, 1); WriteU32(p + 0x1C, 0x4A); WriteU32(p + 0x20, 0x01);
}
IMP(XGetVideoMode)     { TRACE(XGetVideoMode); FillVideoMode(ctx.r3.u32); }
IMP(VdQueryVideoMode)  { TRACE(VdQueryVideoMode); FillVideoMode(ctx.r3.u32); }
IMP(VdQueryVideoFlags) { TRACE(VdQueryVideoFlags); ctx.r3.u64 = 0x3; }   // widescreen | 1280 wide
IMP(VdIsHSIOTrainingSucceeded) { TRACE(VdIsHSIOTrainingSucceeded); ctx.r3.u64 = 1; }
IMP(VdInitializeEngines) { TRACE(VdInitializeEngines); ctx.r3.u64 = 1; }
IMP(VdShutdownEngines) { TRACE(VdShutdownEngines); }
IMP(VdEnableDisableClockGating) { TRACE(VdEnableDisableClockGating); ctx.r3.u64 = 0; }
IMP(VdGetCurrentDisplayGamma) { TRACE(VdGetCurrentDisplayGamma); if (ctx.r3.u32) WriteU32(ctx.r3.u32, 2); float g = 2.22222233f; uint32_t b; memcpy(&b, &g, 4); if (ctx.r4.u32) WriteU32(ctx.r4.u32, b); }
// VdGetCurrentDisplayInformation(X_DISPLAY_INFO*) - 0x58 bytes; front buffer size, display size, refresh
IMP(VdGetCurrentDisplayInformation) {
    TRACE(VdGetCurrentDisplayInformation);
    uint32_t p = ctx.r3.u32; memset(g_base + p, 0, 0x58);
    auto w16 = [](uint32_t a, uint16_t v) { g_base[a] = v >> 8; g_base[a + 1] = v & 0xFF; };
    w16(p + 0x00, 1280); w16(p + 0x02, 720);          // front buffer
    g_base[p + 0x04] = 0; g_base[p + 0x05] = 0;
    w16(p + 0x08, 0); w16(p + 0x0A, 0); w16(p + 0x0C, 1280); w16(p + 0x0E, 720);
    w16(p + 0x10, 1280); w16(p + 0x12, 720);          // display
    WriteU32(p + 0x40, 320); WriteU32(p + 0x44, 180); // actual display size / 4
    WriteU32(p + 0x50, 1280); WriteU32(p + 0x54, 720);
}
IMP(VdSetSystemCommandBufferGpuIdentifierAddress) { TRACE(VdSetSystemCommandBufferGpuIdentifierAddress); }
IMP(VdPersistDisplay) { TRACE(VdPersistDisplay); if (ctx.r4.u32) WriteU32(ctx.r4.u32, 0); ctx.r3.u64 = 1; }
IMP(VdRetrainEDRAM) { TRACE(VdRetrainEDRAM); ctx.r3.u64 = 0; }
IMP(VdRetrainEDRAMWorker) { TRACE(VdRetrainEDRAMWorker); ctx.r3.u64 = 0; }

IMP(VdSetDisplayMode) { TRACE(VdSetDisplayMode); ctx.r3.u64 = 0; }
IMP(VdInitializeScalerCommandBuffer) { TRACE(VdInitializeScalerCommandBuffer); ctx.r3.u64 = 0; }
