# Known issues and validation limits

- 01.52 is a playtest candidate. The last identity-verified device state was 01.50; newly generated assets and this candidate still need physical acceptance.
- Crowded battles and multi-level dungeons can remain slow. No minimum FPS or new 01.52 speed gain is claimed. Prior console/map-loading crashes are not declared fixed by this packaging update.
- Adaptive resolution responds to rendering pressure and can remain native when AI/movement is the bottleneck.
- Diagnostics are opt-in and can slow the game. Compare capture-OFF runs separately from capture-enabled diagnosis.
- The Shared actor helper pool test switch requires restarting the game; the baseline keeps it and Tighter indoor queries off.
- Builder 0.3.0 accepts the exact verified English GOG files listed in its resources. Other editions/languages/revisions and modified originals are not supported silently.
- All 53,216 generated nonvideo files match the prepared 01.51 runtime. Videos are locally re-encoded; all 438 unique clips passed complete host decode and timing/audio checks; a physical playthrough remains unverified.
- GUI construction/rendering/bounds were checked; full conversion was run through the bundled backend, not an interactive click test.
- Some supplemental media provenance remains untraced. No originals-free supplement or legal clearance is claimed.
- Vita3K uses the same files but is not a substitute for physical Vita testing.
- Keep backups of saves before updates or adventure conversions; do not replace your settings with diagnostic preset fragments.

- 01.52 retains existing personal settings. Old testing preferences may still show the resolution label or enable the console until switched off in Vita Settings.
