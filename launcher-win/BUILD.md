# Windows launcher

Native Win32 launcher with two tabs:

- **Play**: pick the game folder or XBLA package, then press Play. It runs `madden_host.exe` from the same folder with
  `--settings settings.ini`, and shows and saves the output (`madden_log.txt`).
- **Settings**: finds Vulkan GPUs through volk (runtime loader, no SDK needed) and recommends renderer and quality:
  - discrete GPU with 6 GB or more and a 1440p+ screen → High (4K internal);
  - discrete GPU with 3 GB or more → Medium (2K internal);
  - anything else → Low (720p).

  You can also set the internal resolution (1x/2x/3x), anisotropic filtering, fullscreen or windowed, window size, VSync
  and the FPS counter. Settings are saved to `settings.ini` (`[settings]` key=value); `host/src/settings.cpp` reads it.

Cross-compile from Linux. The volk and Vulkan-Headers paths are checkouts of
https://github.com/zeux/volk and https://github.com/KhronosGroup/Vulkan-Headers:

    x86_64-w64-mingw32-windres launcher.rc -O coff -o launcher_res.o
    clang --target=x86_64-w64-mingw32 -O2 -c -I$VKHEADERS/include $VOLK/volk.c -o volk_win.o
    clang++ --target=x86_64-w64-mingw32 -std=c++20 -O2 -municode -mwindows -I$VOLK -I$VKHEADERS/include \
        launcher.cpp launcher_res.o volk_win.o -o MaddenArcadeLauncher.exe -static -fuse-ld=lld \
        -lcomdlg32 -lshell32 -lole32 -loleaut32 -lcomctl32 -luxtheme -lmsimg32 -luuid

Set `MADDEN_ALLOW_CPU_GPU=1` to list software Vulkan devices such as lavapipe (useful for testing under Wine).

Background picture: put any JPG named `launcher_bg.jpg` next to the launcher (it is not shipped; without one a plain dark-blue backdrop is drawn).
