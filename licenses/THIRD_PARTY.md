# Third-party software

The NRO is built from this repository's code together with the following projects. Their licences apply to
their parts; full texts are in this folder where the project ships one.

| Project | Used for | Licence |
|---|---|---|
| [ShaolinMonksRecomp](https://github.com/OAleex/ShaolinMonksRecomp) by OAleex | the game profile, host adaptations and game-specific fixes this port builds on | GPL-2.0-or-later ([LICENSE](../LICENSE)) |
| [XboxXBERecomp](https://github.com/TGP482/XboxXBERecomp) by TGP482 | the static recompiler and the Xbox runtime (kernel, Direct3D 8, DirectSound, XAPI) | MIT ([XboxXBERecomp-MIT.txt](XboxXBERecomp-MIT.txt)) |
| YabaSanshiro NX overlay, from NaGa's Dolphin NX frontend | the quick menu's model and look (`port/common/quick_menu.*`) | GPL-2.0-or-later |
| [Mesa](https://mesa3d.org) (NVK, the Vulkan driver), Switch Horizon SDK 26.2.4 | Vulkan on the Switch's GPU | MIT and other permissive licences, see [docs.mesa3d.org/license.html](https://docs.mesa3d.org/license.html) |
| [glslang](https://github.com/KhronosGroup/glslang) 15.1.0 | the renderer's HLSL to SPIR-V | BSD-3-Clause, MIT, Apache-2.0 and others ([glslang.txt](glslang.txt)) |
| [Dear ImGui](https://github.com/ocornut/imgui) 1.91.0 | the quick menu | MIT ([DearImGui-MIT.txt](DearImGui-MIT.txt)) |
| [toml++](https://github.com/marzer/tomlplusplus) 3.4.0 | `settings.toml` | MIT ([tomlplusplus-MIT.txt](tomlplusplus-MIT.txt)) |
| [libnx](https://github.com/switchbrew/libnx) and devkitA64 (newlib, libstdc++) | the Switch system library and C/C++ runtime | ISC (libnx); newlib's licences; GPL with the GCC Runtime Library Exception (libstdc++) |
| [Zydis](https://github.com/zyantific/zydis) 4.1.1 | the recompiler, on the build machine only | MIT |

`port/vk/bc_decode.*` is ShaolinMonksRecomp's BC decoder, unchanged apart from its include path.
