// Vulkan backend for Xenos draws (render targets, pipelines, textures, resolves).
#pragma once
#include <cstdint>

struct GpuDrawCall {
    uint32_t initiator;        // VGT_DRAW_INITIATOR
    uint32_t indexBase = 0;    // physical address of the index buffer (DMA indices)
    uint32_t indexSizeInfo = 0;// DRAW_INDX dword 3: count + endian (bits 30-31)
};

// Shared accessors implemented in gpu.cpp.
uint32_t GpuRegister(uint32_t index);
uint32_t GpuSwapValue(uint32_t v, uint32_t endian);

// Renderer entry points (no-ops if the renderer is not active).
bool RendererActive();
void RendererDraw(const GpuDrawCall& call);        // RB_MODECONTROL decides draw vs resolve
bool RendererSwap(uint32_t frontBufferPhys, uint32_t width, uint32_t height);   // true = frame handed to the presenter
