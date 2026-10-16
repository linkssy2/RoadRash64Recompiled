# Credits and acknowledgements

This project exists because of the original Road Rash 64 development team and the work of the following authors and communities. The maintainer claims no credit for their games, tools, libraries, or artwork. Credit is not a claim of endorsement. Individual copyright notices are preserved in the license bundle and upstream repositories.

| Project / material | Authors and contributors | Use |
|---|---|---|
| Road Rash 64 | Original game developers, artists, composers, and other rights holders | The game being preserved; player-supplied ROM |
| [N64Recomp / RSPRecomp](https://github.com/N64Recomp/N64Recomp) | Wiseguy and contributors | CPU and audio-microcode translation |
| [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) | N64Recomp runtime contributors | Native runtime, virtual N64 services, ROM loading, saves, mods |
| [RT64](https://github.com/rt64/rt64) | RT64 authors and contributors | Graphics, enhancement and presentation foundation |
| [RecompFrontend](https://github.com/N64Recomp/RecompFrontend) | RecompFrontend authors and contributors | Menus, controls and configuration |
| [Zelda 64: Recompiled](https://github.com/Zelda64Recomp/Zelda64Recomp) | Zelda64Recomp contributors | Reference architecture and project guidance; not an endorsement |
| [BanjoRecomp](https://github.com/BanjoRecomp/BanjoRecomp) | BanjoRecomp contributors | Generic frontend SVG icon provenance recorded in this project |
| [Plume](https://github.com/renderbag/plume) | renderbag and contributors | Graphics abstraction |
| [RmlUi](https://github.com/mikke89/RmlUi) | RmlUi contributors | User-interface rendering/layout |
| [SDL](https://github.com/libsdl-org/SDL) | SDL authors and contributors | Windows, input, audio and controller support |
| [Opus](https://opus-codec.org/) | Xiph.Org and the authors named in the Opus license | Experimental proximity-voice codec |
| [FreeType](https://freetype.org/) | FreeType Project | Font rasterization; portions of this software use FreeType under the FreeType License |
| [lunasvg / plutovg](https://github.com/sammycage/lunasvg) | Samuel Ugochukwu and contributors | SVG/vector support |
| [GamepadMotionHelpers](https://github.com/JibbSmart/GamepadMotionHelpers) | Julian “Jibb” Smart and contributors | Controller motion helpers |
| PromptFont | Yukari “Shinmera” Hafner | Controller/keyboard prompt font, SIL OFL 1.1 |
| Lato / LatoLatin | Łukasz Dziedzic and font contributors | Frontend text, SIL OFL 1.1 |
| Noto Emoji | Noto font contributors | Emoji fallback, SIL OFL 1.1 |
| [RetroAchievements Road Rash 64 Base Set](https://retroachievements.org/) | Set and badge contributors | Local achievement names, descriptions and badge imagery; no account integration |
| [Road Rash 64 Remastered Edition](https://crisaty.tumblr.com/post/113116999840/road-rash-64-remastered-edition) | Crisaty (Cristian A.T.) | Original mod and texture artwork; optional RT64 conversion provided as a separate optional release download |

Additional dependency credit belongs to the authors of Dear ImGui, ImPlot, im3d, hlsl++, ddspp, re-spirv, SPIRV-Cross, SPIRV-Headers, Vulkan-Headers, VulkanMemoryAllocator, D3D12MemoryAllocator, volk, nativefiledialog-extended, stb, miniz, zstd, xxHash, fmt, o1heap, Rabbitizer, ELFIO, toml++, SLJIT, concurrentqueue, and other bundled dependency components. Consult their preserved notices and pinned sources for individual names and exact terms. This list does not replace those notices.

Development also used Microsoft Visual Studio/Windows SDK, LLVM/Clang, DirectX Shader Compiler, CMake, Ninja, Git, Python, Pillow and PowerShell. Earlier setup tooling used Go. These tools belong to their respective authors; not all development tools are distributed with the game.

OpenAI ChatGPT/Codex provided AI assistance for development, troubleshooting, documentation and generated/edited launcher artwork. This is disclosed as a development method, not as authorship of the original game or upstream technology. The maintainer is responsible for the decision to use it and for addressing defects and attribution issues.

Please report omissions or incorrect credit so the record can be corrected. No contributor's name should be used to imply their approval of this project or its AI-assisted changes.

## Included optional texture conversion

Road Rash 64 Remastered Edition was created by **Crisaty (Cristian A.T.)**. The separate optional download includes an **RT64 conversion of that original Jabo texture pack** for Road Rash 64 Recompiled. All credit for the original mod and artwork belongs to its creator; the recompilation project does not claim authorship of those textures or endorsement by the creator.

[Original creator and project page](https://crisaty.tumblr.com/post/113116999840/road-rash-64-remastered-edition)

The optional RTZ is in the separate **RoadRash64Recompiled-Optional-Texture-Mod.zip** release download. Install it through Settings > Mods. Replace an older copy rather than installing duplicates. The source ZIP does not include the texture pack. Attribution is not a redistribution license; no explicit redistribution license was found in the supplied pack or linked creator page.

## Native Linux contribution

CalenCyr contributed the Linux build, POSIX compatibility, FFmpeg audio decoding,
shutdown diagnosis and AppImage packaging work in PR #4. The contributor disclosed
substantial Claude AI assistance and reported Fedora and Steam Deck testing. This
experimental integration carries later Road Rash fixes and additional packaging
changes; that earlier testing does not establish acceptance of this binary.

## Optional MK64 course import

Mario Kart 64 and its original courses, artwork and audio belong to their original creators and rights holders. Importing uses the player's own supported ROM; the release does not include that ROM or the resulting course assets.

The importer uses the [n64decomp/mk64 project](https://github.com/n64decomp/mk64/tree/58cfcb022e10f83bc3b889d7e97508cae6837098), revision `58cfcb022e10f83bc3b889d7e97508cae6837098`, for course layouts, display-list and compression formats, collision, animation, path and audio behavior. Its contributors' work made the conversion possible. References include `libmio0`, `n64graphics`, the course/rendering code and the native audio sequencer. The motion helper adapts original path/spline routines; the contact helper uses selected translated Road Rash routines with N64Recomp support. No endorsement is implied, and these credits do not relicense original game code.

Course music and sound effects are rendered locally from the ROM's original sequences and instrument samples by the project's Python converter. It uses NumPy for sample processing; a separate MIDI converter or downloaded soundtrack is not required. The result approximates the original audio processing and is not a bit-exact N64 recording. The Linux player's FFmpeg custom-soundtrack decoder is a separate component, credited below.

| Importer component | Authors and contributors | Use |
|---|---|---|
| [Python](https://www.python.org/) | Guido van Rossum, Python Software Foundation and Python contributors | Portable conversion runtime |
| [NumPy](https://numpy.org/) | NumPy developers and contributors | Sample processing and numerical operations |
| [OpenBLAS](https://github.com/OpenMathLib/OpenBLAS), LAPACK and GCC runtime | OpenBLAS contributors, LAPACK authors and Free Software Foundation contributors | Libraries included in the NumPy wheels; the Linux wheel also includes libquadmath |
| [PyInstaller](https://pyinstaller.org/) | PyInstaller development team and contributors | Freezing the converter and its bootloader |
| [OpenSSL](https://openssl-library.org/) | OpenSSL Project authors and contributors | Python's bundled cryptography/TLS libraries |
| [libffi](https://github.com/libffi/libffi) | Anthony Green, Red Hat and contributors | Python's foreign-function support |
| [FFmpeg](https://ffmpeg.org/) | FFmpeg developers and contributors | Linux custom-soundtrack decoding; not part of MK64 course conversion |

Full importer runtime notices are in `tools/mk64-importer/licenses` in the Windows package and `usr/bin/tools/mk64-importer/licenses` inside the Linux AppImage, with supplemental notices in `licenses/mk64-importer`. The Linux runtime also credits the zlib and XZ Utils authors. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for versions and terms, and `scripts/MK64-IMPORTER-NOTICES.md` in the source package for build details. Original game developers, mod creators, upstream researchers and tool authors retain credit for their respective work.

## Optional More Characters skins

More Characters 2.0.5 contains newly authored, AI-assisted diffuse skin textures
fitted to the native male/female rider models. It supplies no replacement meshes
or extracted ROM textures. The pack is separate from the game downloads; native
models and animation still come from the player's Road Rash 64 ROM. Image
generation and reviewed pixel/UV fitting used OpenAI tools, Python and Pillow.

The character references remain credited to their respective creators and rights
holders: RuneScape (Jagex), Punisher and Ghost Rider (Marvel), Halo and Gears of
War (their original teams and rights holders), Doom (id Software), The Legend of
Zelda (Nintendo), Riddick (its creators and rights holders), and Vin Diesel for
his likeness. These are unofficial fan interpretations; no endorsement or rights
to the underlying characters, names or likenesses are claimed.

## 1.4.6 contributions

[heyitsjames](https://github.com/heyitsjames) contributed All Tracks Available,
three/four-player widescreen and full-HUD improvements, speedometer placement,
and A/Start intro-page skipping in PRs #19, #21, #23, #25 and #27.
