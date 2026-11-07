<h1 align=center>The King of Fighters 2002 Unlimited Match — Nintendo Switch port</h1>

A native Nintendo Switch port of the Xbox 360 (XBLA) version of **The King of Fighters 2002 Unlimited Match**.

It's not an emulator. The PowerPC code of the game's `default.xex` is statically recompiled to C++ with
[ReXGlue](https://github.com/rexglue/rexglue-sdk), compiled for the Switch with devkitPro/libnx, and the game is
drawn through a Vulkan backend running on NVK (Mesa).

> [!NOTE]
> This repository contains **no game data and no code derived from the game**. You need your own copy of the
> Xbox 360 game (the XBLA package).

## Status

Work in progress. In dock, at the console's stock clocks with no overclock (CPU 1020 MHz, GPU 768 MHz, memory
1600 MHz), a full fight runs at ~52–56 FPS (the target is a stable 60). Below 60 the game runs
slightly slower than normal, because its logic advances one step per drawn frame. 
**Known issues:** on the character select screen the character grid is not drawn (the cursor and the selection
still work). Handheld mode has not been measured yet.

See [PORT_STATUS.md](PORT_STATUS.md) for the detailed log: newest section first.

## Layout

| Path | What it is |
|---|---|
| `src/recompiler/` | ReXGlue manifest, CMake project and the port's own code (hooks, autotest/bench support) |
| `tools/rexglue-sdk/` | ReXGlue SDK with the Switch runtime: Horizon layer, Vulkan/NVK renderer changes, perf counters |
| `tools/fetch_thirdparty.py` | Downloads the SDK dependencies at their pinned commits (`thirdparty_pins.txt`) |
| `tools/kof_stfs_extract.py` | Extracts the XBLA STFS package, even with a missing block (locates blocks by SHA-1) |
| `tools/xctd/` | Decompresses the game's XCTD (LZX) files |
| `tools/ghidra-scripts/` | Headless Ghidra helpers used for reverse engineering |
| `switch/kof2002um_nx.toml` | Runtime settings that go next to the NRO (the current 60 FPS-oriented set) |
| `bench/devkitnx-configs/` | Configs for the automated on-console benchmark (devkitnx) |

## Building (outline)

1. Install devkitPro (devkitA64 + libnx) and fetch the SDK dependencies: `python tools/fetch_thirdparty.py`.
2. Extract your XBLA package: `python -I tools/kof_stfs_extract.py <package> game/`, then decompress the XCTD
   files with `tools/xctd/`.
3. Run ReXGlue codegen on your `game/default.xex` with `src/recompiler/kof2002um_nx_manifest.toml`
   (output goes to `src/recompiler/generated/`, which is never committed).
4. Configure and build `src/recompiler` with the Switch preset of `CMakePresets.json`.

## Install on the Switch

```text
/switch/kof2002um-nx/
  kof2002um-nx.nro
  kof2002um_nx.toml
  game.nxpack + game.nxidx   (the game files, made from your copy)
```

Launch in full application mode (hold **R** while starting an installed title, or a forwarder). Album applet mode
does not provide enough memory.

## Credits

- [ReXGlue](https://github.com/rexglue/rexglue-sdk) and the [Xenia](https://github.com/xenia-project/xenia)
  project, whose GPU and kernel code the runtime derives from.
- [nfsmw-nx](https://github.com/StevensND/nfsmw-nx), another ReXGlue port to the Switch.
- devkitPro, libnx and Mesa/NVK.
