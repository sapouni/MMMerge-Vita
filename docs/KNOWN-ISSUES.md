# Known issues and test status

- 01.06 installs and runs; its old OYAM00001 LiveArea shows only the bubble icon.
  The identical art displays correctly in the separate OYAMART01 test app.
  01.06 R2 uses a new MMVITA001 game identity; that full-game bubble awaits hardware confirmation.
- Triangle spell assignment, cap behavior and the map-cache change passed host checks;
  specific hardware results are not yet reported.
- Crowded battles, multi-level caves and some transitions can remain slow. No guaranteed FPS target is advertised.
- Adaptive world resolution can lower 3D clarity; HUD text stays at native resolution.
- Record diagnostics only when needed; capture overhead is not a gameplay benchmark.
- Exact future upstream asset compatibility has not been established. Keep backups of saves before updates.
- The port uses a modified ARMv7/GLES2 path; updating to current upstream is not a drop-in library replacement.

No gameplay changes were made for the separate OYAMART01 artwork-check app.
