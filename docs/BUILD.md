# Current source/build status

The 01.52 player release is a standalone VPK. Its exact source, symbols and
configuration are retained locally. The 01.51 release provides the frozen engine/game/Vita source snapshot and the
corresponding bgfx source snapshot as separate release ZIPs. The older checked-in
openyamm/dependency tree below remains historical 01.06 material; do not assume
that checking out the release tag and running those historical commands yields
the current VPK. A fresh-machine one-command 01.51 build kit is not claimed.

The new builder source is in builder/. Its versioned data manifests, source
patches and supplemental resources ship in the portable release rather than
being added to Git history. Python/FFmpeg/runtime notices accompany that release.
The standalone tool uses its bundled isolated Python and requires no Python
installation. Resource preparation scripts describe the developer pipeline and
expect its reference inputs; users simply run the released GUI.

The cancelled PC-sampler project remains stopped; no profiler is installed or
required by these downloads. Diagnostics remain opt-in.

## Historical 01.06 build notes

# Source and build status

The `openyamm/` tree is the complete code/configuration snapshot of the delivered 01.06 port.
Original-game archives, generated asset packs, personal saves, crash dumps and private captures are excluded.
`deps/` contains dependency source/configuration text used by this workspace, including modified bgfx,
PhysicsFS, SDL and the selected vitaGL variant. Binary test media and generated build products are excluded.
`loader-deps/` contains the native shared-library loader and kubridge headers/source with their notices.

This is source for review and continued development, not yet a clean-machine reproducible build kit.
Historical commands under `vita/build-provenance/` show the actual successful flags and linkage;
`${WORKSPACE}` replaces the original absolute workstation path. The accompanying scripts still expect
the established workspace's configured build trees and libraries. Do not run them as a fresh-clone bootstrap.

Build environment: Android NDK r28c, ARMv7/API23, GLES2 shader variants, bgfx, SDL3 and C++20;
native VitaSDK softfp, matching SDL/vitaGL ABI and `-fno-short-enums`. The native CMake entry point is
`openyamm/packaging/vita`, with `PORT_WORK` and `PORT_VITAGL_LIBRARY` selecting its prepared dependencies.
The tested library variant is `deps/vitagl-safe-clean-0054`, not the unmodified upstream vitaGL master.
Build host shaderc first, compile the Vita shader variants, then the Android guest and native bridge.
Use the source manifests and source checks to retain matching headers/layouts. Check guest imports against
the bridge before packaging; so_util needs ELF sections, so indiscriminate stripping is unsafe.

The old PC sampler remains disabled and is omitted from this upload. There is no automatic build or
scheduled workflow in this repository. The original upstream release workflow, which packages game assets,
is deliberately excluded. Publishing a release is a manual operation.

Before a public developer release, turn the historical provenance into a fresh-checkout build script,
pin all external downloads and validate that build on another machine. Never claim this has already passed.
