# Building the setup

`setup.exe` = the wizard (`setup.cpp`) with the runtime pack (a zip) built in as the `PACK` resource. The pack holds
only this project's own code and open-source parts, never game code:

| Folder | Contents |
|---|---|
| `tools/XenonRecomp.exe` | XenonRecomp (MIT) with `patches/xenonrecomp_madden.patch`, built for Windows |
| `recomp/` | `config/arcade.toml.in` and the jump-table address list (`switch_tables.toml`, from `tools/findswitch.py`) |
| `include/` | XenonUtils headers and SIMDe (MIT) for compiling the recompiled code |
| `lib/` | `libmadden_host.a` (the host runtime + volk + tiny-AES), glslang, FFmpeg (LGPL-2.1) static libraries |
| `app/` | `MaddenArcadeUnleashed.exe` (the launcher) |
| `licenses/` | licenses of the parts above |

Everything is built with llvm-mingw 20261117 (the same toolchain the setup downloads, so the libraries match):
the host runtime from `host/src` (+ the generated import stubs), glslang and FFmpeg (`tools/build_ffmpeg.sh` options)
with that toolchain, then `windres setup.rc` and link with `-lcomctl32 -lcomdlg32 -lshell32 -lole32 -luuid -lurlmon
-lbcrypt -lmsimg32`.

On the player's PC the setup: unpacks the pack, unpacks/checks the game (Arcadegame.dll SHA-1), downloads llvm-mingw
(SHA-256 checked), runs XenonRecomp once with the shipped config, compiles the generated C++ on all cores, links
`madden_host.exe` against the pack's libraries, writes settings / mods, and deletes the generated code.
