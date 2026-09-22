# Maintaining the Vita port

The current game source base is upstream commit `db142fb3f38fb59be7f5a50a393dbe4377772bbe`.
The local Git snapshot commit is `65dec93f6323e9b7e1bf6783f0e6de3873686d8e`; it intentionally excludes game assets.
Do not use that local snapshot hash as an upstream GitHub revision.

The Vita build compiles Android ARMv7 with `OPENYAMM_VITA_LOADER=ON`, GLES2 shaders and a separate native loader.
Keep platform changes inside this option where practical. Shared ownership or streaming fixes may affect other builds
and require their focused tests. Native ABI shims belong in `packaging/vita`. Dependency corrections are separate patches.

The exported patch bundle contains:

- `openyamm.patch`: tracked game/engine/build changes and newly added port sources against the pinned source base.
- `bgfx.patch`, `physfs.patch`, `vitagl.patch`: dependency changes, kept separate from game updates.
- `manifest.json`: upstream identity, file hashes, dependency origins and patch replay results.

An upstream update is a source and asset compatibility change, not just a replacement Android library. Apply patches
to an isolated checkout of the new upstream revision. Use the pinned base for three-way merges; resolve conflicts explicitly.
Never merge into the only working Vita checkout or overwrite the last matching debug files while reviewing an update.

After a merge, rebuild ARMv7 release and GLES2 shaders together, verify all imports against the native bridge, compare
the shader file list and bytes with the installed package, and run the streaming, ownership, terrain and upload tests.
Verify menu, character creation, a rendered map, save/load and a second map transition on the physical Vita before
marking the updated port working. Preserve each version's native ELF, Android unstripped library and deployed hashes.

On 2026-09-11 the reviewed upstream head was `504fb38fed5e2d3a0caffb062f6d40808ac8156c`, five commits after our base.
The update includes broad gameplay/rendering changes and Android shader installation changes. Its Android build now
targets GLES3 shaders, which cannot be applied unchanged to this Vita GLES2 build. Our native loader reads packaged
shaders directly and does not use Android's APK-to-data shader extraction path. The new upstream shader-copy fix therefore
does not directly repair the Vita's graphics allocator overwrite. These updates have been reviewed, not merged into 00.26.

Screen resolution remains 960x544. A future lower-resolution mode must coordinate the native framebuffer, SDL/bgfx
drawable dimensions, view rectangles and pointer/touch scaling. It can reduce render-target memory and fill cost;
it does not repair mipmap bounds or reduce all source texture and world-data allocations.

00.27 additionally builds both native vitaGL variants with `HAVE_SHADER_CACHE=1` and `SOFTFP_ABI=1`;
the emulator also uses `NO_SPLASHSCREEN=1`. Cache root is `ux0:data/openyamm/shader-cache-v1`, followed by
vitaGL's serialization-version and shader-stage directories. Shader work is still required on a cold cache.
Keep cache namespaces distinct when changing shader translation/compiler options incompatibly.
`gpu_alloc_mipmap_storage` now separates empty image storage from generated mip pixels; update its internal header too.
The game sets the `openyamm_vita_gameplay` SDL hint to gate native keyboard/camera mappings; generic menus retain the pointer.

00.28 uses a streaming case-insensitive filename search and a Vita-only bounded cache for immutable archive mounts.
Invalidate the cache when search mounts change; directory mounts disable it. Preserve the old sorted-choice behavior
when differently cased names coexist. The native loader requests CPU/bus/GPU/crossbar clocks of 444/222/222/166 MHz
and logs the actual values. Routine diagnostics avoid graphics-heap free-space scans and record callback/submission
and render-CPU durations instead. Neither clock selection nor these timings establish a playable frame rate.

00.29 adds `OPENYAMM_VITA_SHADER=1` only to Vita shader compilation. World lights, radial fog distance and secret
pulse move to vertices; procedural water/lava distortion is reduced, while animated water textures remain. Billboard
selection uses four cardinal outline samples. Compile and deploy all runtime ESSL binaries alongside the guest library.
Non-Vita shader branches retain the original per-pixel effects. Visual and performance validation remains required.

Native vitaGL patches now also include buffers.c and gxm.c: allocation recovery waits for garbage-collector completion
instead of sleeping one second, failed buffer subdata allocation returns GL_OUT_OF_MEMORY, and slow swaps log separate
display-queue and cleanup waits. GPU completion checks remain enabled. Emulator builds now use HAVE_SHADER_CACHE=0
and NO_SPLASHSCREEN=1; physical builds retain HAVE_SHADER_CACHE=1. Both require SOFTFP_ABI=1. The emulator cache bypass
is an unconfirmed workaround, not a demonstrated crash fix. Runtime sprite texture growth remains an open memory risk.
