MMMerge Vita Asset Builder 0.3.0 - Windows x64
Game-data target: the complete 01.51 runtime set (53,745 files).

USE
1. Extract this entire tool ZIP to a folder on the PC. Do not run it from inside
   the ZIP. Keep the _internal folder beside the two executable files.
2. Install the original English GOG releases of Might and Magic VI, VII and VIII.
   Select each installed game folder, not its Data subfolder or installer.
   No separate MMMerge mod, OpenYAMM download, Python installation or FFmpeg
   download is required. FFmpeg is bundled for local video conversion.
3. Open "MMMerge Vita Asset Builder.exe". Select all three installed games and
   a new or empty output folder. Allow at least 8 GiB free on that output drive.
4. Press Build complete game data. Input verification, extraction, local project
   patches, videos, file validation and archive verification run in order.
   Videos can take several minutes. Wait for the completion message.
5. Copy the resulting engine.zip and worlds folder into:
      ux0:data/openyamm/assets/
   Leave the five inner ZIPs packed. Install the separate MMMerge Vita VPK.
   INSTALL-DATA.txt and MMMerge-build-report.json describe and verify the output.
   The same archives are used on Vita and Vita3K. No personal saves/settings or
   plugins are written by this tool. It never connects to a Vita or the internet.

SUPPORTED INPUTS
This release accepts the exact English GOG container/music files recorded in
_internal/data/supported-inputs.csv. Different language editions, modified mod
installations, CD releases and other GOG revisions are not silently accepted.
The tool identifies a missing or mismatched input before building. Use clean
installations; a filename resemblance is not sufficient to ensure compatibility.

SAFETY / FAILURE
The original installations are read only. Output may not overlap them or contain
existing files. Generation uses a temporary sibling folder. Only a fully verified
set is moved to the output folder on success; conversion errors do not publish
partial archives. Ordinary errors clean temporary files. If Windows or the PC is
forcibly stopped, an mmmerge-build-* temporary folder and .<output-name>.mmmerge-
build.lock can remain beside the chosen output. After confirming no builder or
FFmpeg process is running, remove those specific leftovers before retrying.
Do not delete any original game or existing save folders. Save log exports the
visible progress/error text for troubleshooting. The report contains source
folder paths; review it before sharing the report publicly.

WHAT IS BUNDLED
Original images, audio, music, geometry and videos covered by the local recipes
are reconstructed from the three installed games. The compact patch collection
requires verified original bytes and recreates the exact merged output; it does
not substitute clean original geometry for modified merged geometry.
The supplemental project-data pack provides merged scripts, registries/tables,
scene state, extra voices/art/maps and remaining residual media. Its inventory
is explicit in _internal/data/project-data.csv. Some residual media provenance
is still untraced; this release is NOT claimed to contain zero original-derived
payload. It ships no original game installers or original LOD/SND/VID archives.
resource-summary.json gives the measured local-vs-supplement split. Existing
notices/licenses are retained; calling this a fan project does not change them.

VALIDATION
Every nonvideo output must match the prepared 01.51 runtime size and SHA-256.
All video sources are verified and transcoded locally; encoded video hashes can
differ from the existing prepared set. The complete path set and ZIP CRCs are
checked before publication. End-to-end host conversion does not establish a
physical Vita playthrough or universal compatibility with untested inputs.

COMMAND LINE (optional)
MMMergeBuilderCore.exe --self-test
MMMergeBuilderCore.exe --mm6 "C:\Games\MM6" --mm7 "C:\Games\MM7" --mm8 "C:\Games\MM8" --output "C:\MMMerge data"
--video-workers 1..4 changes the PC's video-conversion parallelism (default 2).
It has no effect on Vita gameplay or fourth-core settings.

The app/VPK is supplied separately. This tool does not update the game engine,
install a plugin, modify firmware, host game data or change any gameplay rules.
