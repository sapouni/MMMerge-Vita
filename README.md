# MMerge Vita

![MMerge Vita](docs/images/logo.png)

A work-in-progress PS Vita port of [OpenYAMM](https://github.com/pjasicek/openyamm),
bringing Might and Magic VI, VII and VIII together through its MMerge-inspired worlds.
It runs an ARMv7 Android engine through the community shared-library loader and vitaGL.

This repository is being prepared **privately**. It is not an official OpenYAMM release.

## Current package: 01.06 R2

Boots and runs on the tester's physical Vita. Menus, save/load, the Vita keyboard,
world selection, controller bindings and the in-game guide are implemented.
Performance depends heavily on the scene; large crowds and some caves remain demanding.
Do not assume a stable frame rate or complete game compatibility from a successful boot.
The artwork displays correctly in the separate test app. The 01.06 R2 package uses
a fresh game identity (MMVITA001) to avoid the old bubble registration problem.
The game binaries match 01.06; the new full-game bubble still needs hardware confirmation.

## Download and install

The installer ZIP will be attached to a private GitHub Release after account setup.
It contains a VPK plus instructions. Game-data archives are **not** distributed here.
See [installation instructions](docs/INSTALL.txt) and the [upstream project](https://github.com/pjasicek/openyamm)
for the prepared asset layout. These ZIPs are not unchanged GOG installation files.

Use `ux0:data/openyamm/assets` for the five prepared data archives. Saves, settings
and derived caches live under `ux0:data/openyamm`. Updating the VPK keeps data separate.

## Controls

| In gameplay | Action |
|---|---|
| Left / right stick | Move / look |
| X / Square | Interact / attack |
| Circle / Triangle | Quick spell / jump |
| R / L | Inventory / inspect target |
| D-pad up / down / left / right | Map / rest / spellbook / next character |
| Select / Start | Turn-based combat / game menu |

In the spellbook, X selects a learned spell and Triangle assigns or clears its quick-spell slot.
Circle inspects there; in gameplay it casts, retaining attack fallback when unassigned.
Vita Settings provides editable D-pad/L shortcuts and explains restart requirements.

## Testing and development

Normal gameplay has diagnostics off. Optional captures are for diagnosis and can be slower;
do not compare diagnostic FPS directly with normal FPS. The starting island battle and
busy caves are target cases. Report the version, settings, location and reproduction steps.

Read [known issues](docs/KNOWN-ISSUES.md), [build notes](docs/BUILD.md),
[01.06 changes](docs/01.06-changes-and-audit.md) and [source attribution](LICENSE-NOTICE.md).

The source snapshot is based on OpenYAMM revision
`db142fb3f38fb59be7f5a50a393dbe4377772bbe`, with Vita modifications.
Dependency source snapshots are included for review; this is not yet a verified one-command clean-machine build.

## Credits

OpenYAMM: pjasicek and contributors. MMerge inspiration: Rodril and the MMerge community.
Vita homebrew infrastructure includes VitaSDK, vitaGL, SDL, kubridge and the community so-loader.
Individual dependencies retain their notices. Might and Magic names and original game artwork
belong to their respective rights holders. This project is not affiliated with them.
