# Known issues and validation limits

- 01.53 and 01.54 have passed local build and regression checks. The last identity-verified device capture was 01.50 with recording enabled. Both newer builds and a full playthrough with generated game data still need hardware testing.
- Crowded battles and multi-level dungeons can remain slow. Vita FPS gains and previous console or map-loading crash reports still need hardware checks.
- Adaptive resolution responds to rendering pressure and can remain native when AI/movement is the bottleneck.
- Diagnostics are opt-in and can slow the game. Compare capture-OFF runs separately from capture-enabled diagnosis.
- The Shared actor helper pool test switch requires restarting the game; the baseline keeps it and Tighter indoor queries off.
- Builder 0.3.0 accepts the exact verified English GOG files listed in its resources. Other editions/languages/revisions and modified originals are not supported silently.
- All 53,216 generated nonvideo files match the prepared 01.51 runtime. Videos are locally re-encoded; all 438 unique clips passed complete host decode and timing/audio checks; a physical playthrough remains unverified.
- GUI construction/rendering/bounds were checked; full conversion was run through the bundled backend, not an interactive click test.
- Some supplemental media provenance remains untraced. No originals-free supplement or legal clearance is claimed.
- Vita3K uses the same files but is not a substitute for physical Vita testing.
- Keep backups of saves before updates or adventure conversions; do not replace your settings with diagnostic preset fragments.

- Updates retain existing personal settings. Old testing preferences may still show the resolution label or enable the console until switched off in Vita Settings.
