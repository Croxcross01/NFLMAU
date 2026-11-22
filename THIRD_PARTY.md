# Third-party code and credits

| Component | License | How it is used |
|---|---|---|
| [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) | MIT | Static recompiler (patched, see `patches/`); fetched by `setup.py`, not stored here |
| [Xenia](https://github.com/xenia-project/xenia) | BSD-3-Clause | Reference for the GPU (Xenos microcode, texture tiling, mip layout, render targets), XMA context handling and audio downmix; ported logic is credited in the source comments |
| [Xenia's FFmpeg fork](https://github.com/xenia-project/FFmpeg) | LGPL-2.1 | XMA decoding (`xmaframes` decoder); built by `tools/build_ffmpeg.sh`, linked statically, not stored here |
| [glslang](https://github.com/KhronosGroup/glslang) | BSD-3-Clause / others | GLSL to SPIR-V at runtime; fetched by CMake |
| [volk](https://github.com/zeux/volk), [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | MIT / Apache-2.0 | Vulkan loader and headers; fetched by CMake |
| SDL2 (Linux only) | zlib | Window and audio on Linux |

No game files, assets, roster data or recompiled code are part of this repository: everything game-derived is
produced on the user's machine from their own dump.

Because FFmpeg is LGPL, distributed builds must allow relinking against a different FFmpeg build (ship the object
files or link FFmpeg dynamically) and include the LGPL license text.
