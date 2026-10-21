# Shaolin Monks NX

A Nintendo Switch port of **Mortal Kombat: Shaolin Monks**, built on [ShaolinMonksRecomp](https://github.com/OAleex/ShaolinMonksRecomp),
OAleex's static recompilation of the original Xbox game. The game's x86 code is translated to C++ ahead of time by
[XboxXBERecomp](https://github.com/TGP482/XboxXBERecomp) and compiled for the Switch's ARM CPU; its Direct3D 8
graphics are drawn with Vulkan through Mesa's NVK driver. This is not an emulator.

You need your own copy of the game. Nothing of the game is in this repository or its releases.

## Status

- Runs the story at full speed (60 fps), with audio. Tested in handheld mode; docked and Ko-op are untested.
- Saving and loading through the game's save shrines.
- A quick menu with cheats, save slots and an FPS counter.
- The game's button prompts show Switch buttons.

## Requirements

- A Switch running Atmosphère.
- The game: **Mortal Kombat: Shaolin Monks (USA, Europe) (En,Fr,De,Es,It)** for the original Xbox, as a disc image
  (`.iso`, Xbox format) or its extracted files. Its `default.xbe` must have the SHA-256
  `48240902bf4a04eb89376217c529531032d3370749bcce91a194878cc49db74a`; the port checks it.
- Start the NRO as an **application**, through a title override (hold R while starting a game) or a forwarder.
  From the album (applet mode) there is not enough memory.

## Installing

Copy into `/switch/shaolinmonks/` on the SD card:

```
/switch/shaolinmonks/shaolinmonks.nro
/switch/shaolinmonks/<the game>.iso        or the extracted files in /switch/shaolinmonks/game/
```

The first start writes `settings.toml` there, and makes these folders:

| Folder | Holds |
|---|---|
| `hdd/` | the Xbox hard disk: saves are in `hdd/E/UDATA/4d570029/`, with backups of the last saves in `hdd/.backups/` |
| `slots/` | the quick menu's save slots |
| `cache/` | compiled shaders and pipelines (the first visit to an area compiles them; later runs are smoother) |
| `logs/` | the log of the last four runs (`shaolin-recomp.log`, then `.1` to `.3`), and `crash.log` after a crash |

## Controls

Buttons are mapped by position, as on the Xbox pad:

| Switch | Xbox | In the game |
|---|---|---|
| B (bottom) | A | jump, confirm |
| A (right) | B | cancel |
| Y (left) | X | |
| X (top) | Y | |
| L / R | White / Black | R kneels at a save shrine |
| ZL / ZR | left / right trigger | |
| + / − | Start / Back | |
| sticks and their clicks | the same | |

The game's on-screen prompts are redrawn with Switch buttons in those places, so the prompt shows the button to
press. The quick menu can switch them back to the Xbox icons.

**Saving:** save shrines are the statues with lights. Stand at one and press **R** (the Xbox's Black button) to
kneel; kneeling also restores health.

## Quick menu

Hold **−** and press **+** to open it; the game sees no input while it is open.

- **Save slot / Load slot:** four slots, each a copy of the game's saves. Save in the game first, then copy it to a
  slot; to go back, load a slot, then choose *Load Game* on the main menu. Slots hold what the game saved, not the
  moment: they are not emulator-style save states.
- **Cheats**, set for player 1 and player 2 separately (the Player row at the top):
  Invincibility, Infinite energy (the blue bar), 999,999 experience, Fatality meter always full, Moon jump (hold
  jump to rise). For both players at once: 10x damage to enemies, Unlock everything (saved with your progress the
  next time the game saves).
- **Show FPS**, and **Button icons** (Switch or Xbox).

## Settings

`settings.toml` in `/switch/shaolinmonks/`:

| Key | |
|---|---|
| `language` | `en`, `fr`, `de`, `es` or `it` |
| `[display] render_percent` | the 3D resolution, in percent of the console's screen height (100: 720 lines handheld, 1080 docked) |
| `[display] vsync`, `fps_cap`, `aniso` | |
| `[game]` | the game's own options (volumes, brightness, Versus rules); `override_original = true` applies them over the save |
| `[switch] button_icons` | `switch` or `xbox` |
| `[switch] guest_core`, `guest_priority`, `render_core` | where the game's threads and the renderer run |
| `[switch] renderer` | `status` runs without graphics, showing a status screen (for diagnosing) |

## Building

On Linux, with:

- [devkitPro](https://devkitpro.org) with devkitA64 and libnx (`DEVKITPRO`),
- Clang 19 (the recompiled game code is built with it: devkitA64's GCC stops with an internal error on it),
- the Mesa Switch Horizon SDK 26.2.4 with NVK (`portlibs/switch` with `lib/libvulkan.a` and the Vulkan headers),
- glslang 15.1.0 built for the Switch **with `cmake/switch.cmake`** (`-DENABLE_HLSL=ON -DENABLE_OPT=OFF`, static),
  so it gets local-exec TLS: GCC miscompiles its thread-local data otherwise,
- CMake, Ninja and Python 3.

```sh
git clone --recursive https://github.com/Thorhax/ShaolinMonks-NX
cd ShaolinMonks-NX
tools/codegen.sh /path/to/default.xbe          # translates the game to C++ in build/generated (not to be shared)
cmake -S . -B build/nx -G Ninja -DCMAKE_BUILD_TYPE=Release -DPORT_PLATFORM=switch \
      -DCMAKE_TOOLCHAIN_FILE=cmake/switch.cmake -DDEVKITPRO=/opt/devkitpro \
      -DNVK_SDK=/path/to/mesa-sdk/portlibs/switch -Dglslang_DIR=/path/to/glslang-nx/lib/cmake/glslang
cmake --build build/nx                         # build/nx/shaolinmonks.nro
```

`-DPORT_PLATFORM=linux-vk` builds the same game and renderer for Linux, drawing off screen (Mesa's lavapipe works
without a GPU), with hooks for scripted input, memory dumps and frame captures: the test bed this port was made
with. `third_party/` holds both upstreams as submodules, pinned to the revisions the port is reviewed against; the
build adapts their files into the build folder and never changes them.

## Known limitations

- The output is 720p in both modes; docked, the Switch scales it.
- No emulator-style save states: the recompiled game keeps its place in native call stacks, which a snapshot cannot
  restore. Save slots and the shrines are the way to go back.
- 10x damage is new and lightly tested.
- ShaolinMonksRecomp 0.2.0's mod manager and language packs are not in this version yet.

## Credits

- **OAleex** for ShaolinMonksRecomp, and **TGP482** for XboxXBERecomp, which do the hard part.
- The **Mesa** developers for NVK, and the authors of the Switch Horizon SDK build of it.
- **NaGa**'s Dolphin NX frontend and **YabaSanshiro**, whose in-game menu the quick menu follows.
- **switchbrew** and **devkitPro** for libnx and the toolchain.
- Port by **Thorhax**.

See [licenses/THIRD_PARTY.md](licenses/THIRD_PARTY.md) for every project used and its licence.

## Licence and disclaimer

The code in this repository is licensed under the **GPL-2.0-or-later**, as ShaolinMonksRecomp is; see [LICENSE](LICENSE).
It grants no rights to the original game.

Mortal Kombat and Shaolin Monks are trademarks of Warner Bros. Entertainment Inc.; the game was developed by Midway.
This project is not affiliated with or endorsed by them, Microsoft or Nintendo. The NRO's icon is the game's cover
art. Use your own legally obtained copy of the game.
