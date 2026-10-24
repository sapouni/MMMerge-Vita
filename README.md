# MMMerge Vita

![MMMerge Vita main menu](docs/images/screenshots/main-menu.png)

**Might and Magic VI, VII and VIII on PlayStation Vita, through OpenYAMM.**

[Downloads](https://github.com/sapouni/MMMerge-Vita/releases) · [Install](#installation) · [Build game data](#windows-asset-builder) · [Controls](#controls) · [Credits](#credits)

MMMerge Vita is an unofficial Vita port of [OpenYAMM](https://github.com/pjasicek/openyamm), bringing together the worlds of Might and Magic VI, VII and VIII. Choose Enroth, Antagarich or Jadame as your starting world. Vita controls, remappable shortcuts, a touchscreen aiming option, native-resolution menus and an in-game guide are included.

**Current playtest: 01.51. Windows Asset Builder: 0.3.0.** This is a work in progress. Crowded dungeons can still slow down and console/debug loading can still fail. 01.51 has desktop build/behavior checks; physical acceptance of this exact candidate and the newly generated data remains pending. No minimum framerate is promised. The repository remains private.

## Screenshots

Custom Vita menus and controls alongside the original adventures. Click any image for the full-size screenshot. These captures show the interface and game content; they are not performance benchmarks.

| Remappable Vita controls and touch aiming | Full-screen adventurer's guide |
|---|---|
| [![Vita controls overview](docs/images/screenshots/controls.png)](docs/images/screenshots/controls.png) | [![Adventurer's guide](docs/images/screenshots/guide.png)](docs/images/screenshots/guide.png) |
| **Choose a starting world** | **Separate adventures or one shared party** |
| [![World selection](docs/images/screenshots/world-selection.png)](docs/images/screenshots/world-selection.png) | [![Adventure modes](docs/images/screenshots/adventure-modes.png)](docs/images/screenshots/adventure-modes.png) |
| **Custom map pins saved with your game** | **Exploring the three worlds** |
| [![Custom map markers](docs/images/screenshots/map-pins.png)](docs/images/screenshots/map-pins.png) | [![Outdoor gameplay](docs/images/screenshots/outdoor-gameplay.png)](docs/images/screenshots/outdoor-gameplay.png) |

<details>
<summary>More screenshots: dungeon combat, exploration and loading artwork</summary>

![Dungeon combat](docs/images/screenshots/dungeon-combat.png)

![Dungeon exploration](docs/images/screenshots/dungeon-exploration.png)

![Full-screen loading artwork](docs/images/screenshots/loading-screen.png)

</details>

## Downloads

**Release upload in progress.** The 01.51 app and builder attachments are being published; this notice will be removed once verified.


| Download | Use it for |
|---|---|
| [Game update ZIP — 97 MB](https://github.com/sapouni/MMMerge-Vita/releases) | The Vita app/VPK, instructions, optional presets and diagnostic tools. Existing players normally need only this ZIP. |
| [Windows Asset Builder — 156 MB](https://github.com/sapouni/MMMerge-Vita/releases) | Creates all five required data archives from supported installed English GOG MM6, MM7 and MM8. New players need this plus the game update ZIP. |

Extract the downloaded ZIPs first. Read `START-HERE.txt` in the builder and `READ-ME-FIRST.txt` in the game update. GitHub's automatic **Source code** ZIPs are not the game installer. Prepared original-game archive sets are not release downloads here.

## Requirements

| Requirement | What you need |
|---|---|
| Homebrew-enabled Vita | [VitaShell](https://github.com/TheOfficialFloW/VitaShell/releases) for manual transfers and VPK installation. No firmware update is required for this release on a working setup. |
| **kubridge installed and loaded** | The compatible release used by this port is [bythos14 v0.3.1_hotfix](https://github.com/bythos14/kubridge/releases/tag/v0.3.1_hotfix). Follow its setup instructions and reboot after changing kernel plugins. |
| **libshacccg.suprx** | The extracted shader compiler at `ur0:data/libshacccg.suprx` or `ur0:data/external/libshacccg.suprx`. See [ShaRKF00D](https://github.com/OsirizX/ShaRKF00D#install-instructions). |
| **CapUnlocker — fourth-core plugin** | Recommended for fourth-core jobs; optional to launch the game. Included in the game update ZIP. Install and load [CapUnlocker](https://github.com/GrapheneCt/CapUnlocker), then reboot; see [plugin instructions](docs/PLUGINS.txt). Diagnostics shows whether access and worker jobs actually work. |
| [Game data](#windows-asset-builder) | Create the five required archives from your installed MM6, MM7 and MM8 using the [Windows Asset Builder](#windows-asset-builder), or keep an existing matching prepared set. |
| Storage | Allow about 2 GB on the Vita plus installation/cache/save space. The PC builder needs at least 8 GiB free on its output drive while converting. |

An Android APK, a separate vitaGL installation and the experimental libperf kernel plugin are **not required**. Do not enable the withdrawn libperf plugin from earlier private experiments.

Keep working YAMT/SD2Vita entries intact and back up the active tai configuration before plugin changes. Copying a kernel plugin alone does not activate it. No plugin changes are needed just to update an already working installation.

## Installation

1. Download the game update ZIP and extract it on your PC.
2. If you do not already have matching prepared game data, use the [Windows Asset Builder](#windows-asset-builder) to create it from your installed originals.
3. Copy **engine.zip** and the **worlds** folder into `ux0:data/openyamm/assets/` exactly as shown below. Keep these five inner ZIPs packed.
4. Transfer **MMMerge-Vita-01.51.vpk** from the game update ZIP and install it through VitaShell. Use the VPK for a fresh installation so the game gets its home-screen bubble.
5. Launch the game. Open **Vita settings → Diagnostics → Use test baseline**, confirm, then close/reopen the **game**. Leave diagnostic recording off for ordinary play. The in-game Guide explains controls and the adventure choices.

```text
ux0:data/openyamm/
└── assets/
    ├── engine.zip
    └── worlds/
        ├── mm6.zip
        ├── mm7.zip
        ├── mm8.zip
        └── mmmerge.zip
```

**The `openyamm` folder name is still current.** It is the path used by the 01.51 app, including saves and settings. The project/repository rename to **MMMerge Vita** does not change that folder or app ID **MMVITA001**.

Do not rename the originals' LOD/SND/VID files to these ZIP names or unpack the five generated archives. `ux0` means the Vita's currently mounted application storage, often SD2Vita.

### Updating without losing saves

Back up `ux0:data/openyamm` first, then install the new VPK over app ID **MMVITA001**. Saves and personal settings are outside the installed app and are not included or replaced by the update. Leave working game-data archives in place. Do not replace or delete the whole data folder and do not overwrite your settings with a preset fragment.

Very old installs using **OYAM00001** should move their five archives from the old app folder into the layout above, retaining backups and the old bubble until the new app finds their data. See [detailed installation notes](docs/INSTALL.txt).

### Vita3K

Use the **same VPK and generated data files**. Install the VPK and place the archives in Vita3K's emulated `ux0/data/openyamm/assets`. Do not install the physical-console kernel plugins into the emulator. Hardware Vita is the priority; emulator compatibility is not guaranteed and does not replace physical testing.

## Windows Asset Builder

1. Install the original **English GOG** releases of MM6, MM7 and MM8.
2. Extract the entire builder ZIP. Run **MMMerge Vita Asset Builder.exe**, keeping its `_internal` folder and other files beside it.
3. Select the three installed game folders themselves, **not** their `Data` folders or installers.
4. Choose a **new or empty output folder**, then press **Build complete game data**. Allow at least 8 GiB free on that drive. Video conversion can take several minutes.
5. After success, follow `INSTALL-DATA.txt` in the output and copy the five archives to the Vita. `MMMerge-build-report.json` records the output hashes.

No separate Python, FFmpeg, MMMerge mod installation or OpenYAMM download is needed. The tool reads the original games without modifying them and publishes only a fully verified output set. It never connects to the Vita or changes plugins/firmware.

**Supported inputs are specific:** this version verifies 71 English GOG container/music files by size and SHA-256. Other revisions, languages, CD editions and modified files are rejected with an input error instead of silently producing incompatible data.

Most data is extracted, converted or patched locally from the originals. Bundled supplemental content provides the merged scripts, tables, scenes, extra art/voices, custom maps and remaining residual media. Its inventory is included. Some supplemental-media provenance remains untraced; the tool is not claimed to contain zero original-derived payload. Original installers and original LOD/SND/VID containers are not bundled.

**Validation:** the portable tool completed an end-to-end conversion. All **53,216 nonvideo files** match the prepared 01.51 runtime exactly; **529 videos** are locally re-encoded. Representative clips passed stream/duration/audio/decode checks. Generated data has not yet received a full physical Vita playthrough. See [builder details](docs/ASSET-BUILDER.md).

## Choose your adventure

- **Separate adventures — recommended for newcomers:** each world has its own fresh party. The new-game screen recommends MM7/Antagarich and its island introduction. Scaling starts off at 100%.
- **MMMerge — one shared party:** carry levels, equipment and progress between worlds. Scaling starts on at 100%; adjust it in **Vita settings → Adventures**.

The choice belongs to the new adventure and does not change existing saves. Adventure settings explain the active save, scaling controls and explicit conversion choices. Back up saves before conversions. Custom map pins are stored in your save, and the full-screen Guide covers combat, spells, equipment, potions, companions and Vita-specific controls.

## Controls

**A button mapper is included:** open **Vita settings → Controls** to change the supported bindings, look settings, D-pad shortcuts and L shortcut layer. The Vita diagram updates to show your saved bindings. Use **Show L layer** to view the modifier shortcuts. Updates preserve existing bindings, so your own layout may differ from these defaults.

[![Vita button mapper, current bindings and touchscreen aiming](docs/images/screenshots/controls.png)](docs/images/screenshots/controls.png)

### Default gameplay buttons

| Input | Action |
|---|---|
| Left stick | Move |
| Right stick | Look |
| Touchscreen — drag the right side | Move the camera precisely for aiming and targeting |
| Cross (X) | Interact |
| Square | Attack |
| Circle | Cast the quick spell; attack if no quick spell is assigned |
| Triangle | Jump |
| L | Inspect the target |
| R | Open inventory |
| D-pad Up | Rest |
| D-pad Down | Map |
| D-pad Left | Spellbook |
| D-pad Right | Next character |
| Select | Toggle combat mode |
| Start | Open the game menu |

### Default L combinations

Hold **L** and press the second button. By default this layer works during **real-time gameplay**; turn-based combat keeps L available for inspection. The mapper lets you change when the layer is active and what its shortcuts do.

| Combination | Action |
|---|---|
| L + D-pad Up | Fly up |
| L + D-pad Down | Fly down |
| L + D-pad Left | Land |
| L + D-pad Right | Map |
| L + Cross (X) | Interact |
| L + Circle | Quick spell |
| L + Square | Attack |
| L + Triangle | Rest |
| L + R | Party overview |

### Menus and spells

In menus, move the pointer with the **left stick** and select with **Cross**, or use the **touchscreen**. Open the spellbook, select a learned spell with Cross and press **Triangle** to assign its quick-spell slot; Circle inspects it. The in-game **Guide** covers inventory, combining potions, combat and merged-world changes. Restart only when an option explicitly says it is needed.

## Settings and testing

Use the in-game baseline before controlled testing: **Standard** compiler, existing fourth-core jobs/priority on, **outdoor facts off**, **Tighter indoor queries off**, **Shared actor helper pool off**. The baseline preserves controls, graphics quality and adventure state. The shared-pool switch is an experiment and requires restarting the game; it is not an established speed improvement.

Keep **Record diagnostics**, NPC movement timings and timeline profiling **off** for normal play and FPS comparisons. Use a separate recording run for diagnosis; capture can slow the game. Compare the same save, route and settings. Crowded Alamos/map 180 and the starting-island cannon battle remain useful test scenes; an empty view is not an equivalent benchmark.

Adaptive resolution responds to rendering pressure. It can stay high when enemy movement/AI is the limiting work; lowering pixel count does not reduce those calculations. Restart only when the setting explicitly says so. See [01.51 release notes](docs/releases/01.51.md) and [known issues](docs/KNOWN-ISSUES.md).

## Source and contributions

The [release](https://github.com/sapouni/MMMerge-Vita/releases) includes the frozen 01.51 engine source snapshot and corresponding bgfx source snapshot. [Builder source](builder/) is in this repository; its large versioned conversion resources travel with the builder release rather than in Git history. The older checked-in engine/dependency tree is retained as historical review material, not advertised as a fresh-clone 01.51 build kit. See [build status](docs/BUILD.md).

Report the version, map/save, reproduction steps, changed settings and capture mode when [filing an issue](https://github.com/sapouni/MMMerge-Vita/issues). Back up saves before testing updates or adventure conversions.

## Credits

This port depends on the original game creators, the Might and Magic community and the Vita homebrew scene.

| Contributor / project | Contribution |
|---|---|
| **New World Computing and the original Might and Magic teams** | Might and Magic VI, VII and VIII, originally published by 3DO. |
| **[pjasicek / Tyrkys](https://github.com/pjasicek/openyamm) and OpenYAMM contributors** | The engine this port adapts, plus shared original-engine research and performance advice. |
| **Rodril and the MMMerge contributors** | The merged-world project and community work that inspired OpenYAMM. |
| **[OpenEnroth contributors](https://github.com/OpenEnroth/OpenEnroth)** | Engine research and community work, also acknowledged by OpenYAMM. |
| **[TheFloW / Andy Nguyen](https://github.com/TheOfficialFloW)** | VitaShell, the original kubridge and shared-library loader work. |
| **[bythos14](https://github.com/bythos14/kubridge)** | The extended kubridge implementation used by this port. |
| **[Rinnegatamante](https://github.com/Rinnegatamante/vitaGL) and contributors** | vitaGL, vitaShaRK and Vita graphics infrastructure. |
| **[OsirizX](https://github.com/OsirizX/ShaRKF00D) and contributors** | ShaRKF00D shader-compiler extraction tooling. |
| **[VitaSDK contributors](https://github.com/vitasdk)** | The Vita homebrew toolchain and SDK. |
| **bgfx/bx/bimg, SDL, FFmpeg, PhysicsFS, Lua and other dependency authors** | Rendering, platform support, media, asset access and scripting. Their notices remain with the included sources. |
| **[sapouni](https://github.com/sapouni)** | Vita project coordination, extensive hardware playtesting, bug reports and feedback. |

Development assistance: **OpenAI Codex**, with additional source-review feedback using **Claude**.

Might and Magic names, original game content and artwork belong to their respective rights holders. This is an unofficial community project. See [source attribution and licensing status](LICENSE-NOTICE.md) for the upstream and dependency notices.
