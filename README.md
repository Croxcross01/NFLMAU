# Madden Arcade Unleashed

*A native PC version of Madden NFL Arcade (static recompilation), with gameplay mods.*

An unofficial native PC port of **Madden NFL Arcade** (Xbox 360, XBLA, 2009), made by statically recompiling the
game's PowerPC code into C++ with [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and running it on a
host runtime that stands in for the Xbox 360 system software.

> **Status: playable.** Full matches run natively on Windows with a Vulkan renderer, XMA audio, controllers and
> two-player local play, plus a set of gameplay mods (below). Expect rough edges - please report what you find.

## Download and play

1. Download **`MaddenArcadeUnleashed_Setup.exe`** from the [Releases page](../../releases/latest).
2. Run it, pick your own copy of the game (the XBLA package file or an extracted folder) and your settings.
3. Setup downloads the free build tools (about 190 MB, once), turns *your* copy of the game into the PC version
   on your PC (roughly 5-15 minutes), and opens the launcher. Press **PLAY**.

**Updates install themselves.** Each time the launcher opens it checks this repository (`update/manifest.txt`). New
mods, fixes and launcher changes download as a small pack (a few MB) and the game is re-linked in seconds; the rare
update that changes how the game is translated rebuilds it from your copy automatically. You can turn this off on
the Mods page.

Needs Windows 10 or 11, a graphics card with Vulkan, and your own copy of Madden NFL Arcade (XBLA). The download
contains no game code or game data - only this project's own code and open-source parts.

## Features

**PC port**
- Native Windows build (Vulkan), resolutions up to 4K, fullscreen / windowed, shader and pipeline caches
- XMA audio through FFmpeg, XInput controllers, keyboard fallback, saves
- Launcher: pick your game files, graphics settings, mods, and a log page

**Gameplay mods** (all on by default; each one can be switched off on the launcher's **Mods** page)
- **Full Playbook** - press **SELECT** on the play-call screen for every play in the game's playbooks (about 280
  offense, 220 defense) as play-art cards: LB / RB change formation, pick with A, look at the play before you snap.
  Built from *your* game files the first time you play (Mods page: *Rebuild playbook list*)
- **CPU play calling** from the full playbook too, not just the arcade picks
- **Extra defender (SS)** comes on against Shotgun plays
- **Pass icons on every receiver** - A / Y are added where the arcade game left receivers without a button
- **Play action fix** - you stay on the QB after the fake handoff
- **Players per side** - 5 on 5, 7 on 7 or full lines
- **Pre-game lineup window** - choose who plays; two players each set their own team (START = ready, BACK = swap
  sides), Player 2 in red
- **Two-player Playbook** - each player opens their own playbook with SELECT, picks with X / A / Y, and only
  "READY" is shown to the other player
- **Roster sets** in `mods/rosters/<name>` (the original roster is never changed)
- **Launcher background** - choose any JPG of your own on the Mods page

## You need your own copy of the game

This repository contains **no game code and no game data**. You supply your own dump of the XBLA release
(the `LIVE` package file, or a folder with the extracted files). The build turns *your* copy into a native
program on *your* machine. Do not upload game files, `Arcadegame.img`, or the generated `ppc_recomp.*.cpp`
files anywhere: they are derived from EA's copyrighted game.

Supported version: `Arcadegame.dll` SHA-1 `b4e72f27effceee64734d365ed9bb214b0772633`.

## Building

Requirements: Python 3.10+ with `cryptography` (`pip install cryptography`), Git, CMake 3.20+, Ninja (recommended)
and Clang.
- **Windows:** Visual Studio 2022 with the "Desktop development with C++" workload and the **"C++ Clang tools for
  Windows"** component (the recompiled code needs clang-cl). Run the commands from a *Developer PowerShell*.
- **Linux:** clang 17+.

Easiest: run the Python launcher, pick your dump, press **Build**. Then use the Windows launcher
(`launcher-win/`, see its `BUILD.md`) or run the host directly.

```
python launcher.py
```

Or from the command line:

```
python setup.py --dump "D:\path\to\your\Madden NFL Arcade package or folder"
build/host/madden_host --game build/game
```

`setup.py` will:
1. extract the package (STFS) and check the game files,
2. decrypt `Arcadegame.dll` (XEX2) into a raw image,
3. clone XenonRecomp at a pinned commit and apply `patches/xenonrecomp_madden.patch`,
4. detect the game's jump tables (`tools/findswitch.py`) and recompile all ~127,000 functions to C++,
   fixing function boundaries automatically,
5. build the native host with CMake.

Compiling the generated code takes a while (roughly 15-30 minutes on a typical PC).

## How it works

| Piece | What it does |
|---|---|
| `tools/stfs.py` | Reads Xbox 360 STFS (LIVE/PIRS/CON) packages |
| `tools/xexload.py` | Decrypts/decompresses XEX2 executables |
| `tools/findswitch.py` | Finds `lwzx/mtctr/bctr` jump tables the stock analyser misses |
| `tools/fixfuncs.py`, `extend.py`, `merge.py` | Fix function boundaries so every switch case stays inside its function |
| `patches/xenonrecomp_madden.patch` | Adds ~40 PowerPC instructions XenonRecomp did not support, out-of-function `bdnz`-style branches, and lets explicit function ranges override automatic splitting |
| `config/arcade.toml.in` | Recompiler config: save/restore helper addresses and explicit function ranges (addresses only) |
| `host/` | The runtime: guest memory, kernel objects, threads, waits, file system, printf, loader/system calls |
| `tools/bgfa.py`, `extract_ast.py`, `unpack_nested.py` | Read/extract EA's `BGFA1.05` asset archives (`qkl_*.ast`) for modding |

The game ships as a small launcher (`default.xex`) that loads `Arcadegame.dll` and calls export ordinal 1 as
`main(2, argv)`; the host does the same after running the DLL's CRT startup.

File paths are mapped like this: `game:\`, `d:\` and `\Device\Cdrom0\` go to your game folder (read-only),
while `cache:\` and other writable devices go to the save folder (`--save`, default `./save`).

## Roadmap

- [x] Package extraction, XEX decryption, BGFA archive tools
- [x] Full recompile with zero jump-table errors
- [x] Kernel layer: memory, threads, events, semaphores, mutexes, timers, critical sections, TLS
- [x] File system mapped to your game folder; game opens its archives
- [x] Kernel variables (KeTimeStampBundle etc.), physical-memory aliasing, XAM content/saves, license, device selector
- [x] GPU front end: PM4 command processor, ring buffer, interrupts, vblank, VdSwap (null GPU - no drawing)
- [x] XAudio render driver (paced, silent) and input (XInput + keyboard fallback)
- [x] Graphics: Vulkan renderer (Xenos draws, shaders, textures, render targets), shader/pipeline caches, 4K downscale
- [x] Audio: XMA decoding (FFmpeg) and speaker output (WASAPI/SDL)
- [x] Modding: mods overlay, roster database tools, new teams beyond the 32 NFL teams (see docs/MODDING_TEAMS.md)
- [x] Gameplay mods: full playbook, CPU play calling, extra Shotgun defender, pass icons, play action fix,
      players per side, lineup window, two-player playbook
- [ ] Modding: original uniforms for new teams
- [ ] (Later) online play over a replacement service

## Debugging

`madden_host --trace` (or `MADDEN_TRACE=1`) logs every system call; `MADDEN_FILE_TRACE=1` logs file reads;
`MADDEN_STRICT=1` aborts on the first unimplemented system call. `MADDEN_MOD_DEBUG=1` logs team queries and match setup
for mods. Everything the game prints is also saved to `madden_log.txt` next to the launcher (Log page: *Open log*).

## Mod files

All mod switches are small text files in the `mods` folder next to the launcher (the Mods page writes them):

| File | Meaning |
|---|---|
| `roster.txt` | `original` or a folder name under `mods/rosters` |
| `lines.txt` | `5`, `7` or `full` |
| `lineup_picker.off` | present = no pre-game lineup window |
| `cpu_playbook.txt` | `off` = CPU calls only the arcade plays |
| `extra_db.txt` | `off`, or which defender comes on against Shotgun: `ss` (default), `fs`, `cb` |
| `pa_fix.txt` | `off` = the game's own play action |
| `pass_icons.txt` | `game` = the game's own pass icons |
| `playcall_plays.txt` | the Full Playbook list, built from your game files (delete it to rebuild) |
| `launcher_bg.jpg` (next to the launcher) | your launcher background picture |

## Legal

Not affiliated with or endorsed by Electronic Arts, the NFL, or Microsoft. Madden NFL is a trademark of EA; NFL
team names and logos belong to the NFL. This project only contains original tools and runtime code. You are
responsible for using it with a copy of the game you own.
