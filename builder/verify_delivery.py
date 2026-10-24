"""Independent archive comparison against the exact prepared 01.51 assets."""
from pathlib import Path
import hashlib
import json
import subprocess
import zipfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
REFERENCE = ROOT / 'work/vita3k-runner/portable/fs/ux0/data/openyamm/assets'
OUTPUT = HERE / 'portable final verified data'
PORTABLE = HERE / 'portable/MMMerge Vita Asset Builder'
OUT_ZIP = ROOT / 'outputs/MMMerge-Vita-Asset-Builder-0.3.0-Windows.zip'

def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()

def main():
    if OUT_ZIP.exists():
        raise FileExistsError(OUT_ZIP)
    report = json.loads((OUTPUT / 'MMMerge-build-report.json').read_text())
    assert report['status'] == 'complete' and report['runtime_files'] == 53745
    assert report['reconstruction']['source_input_files'] == 71
    nonvideo = video_exact = video_changed = 0
    comparisons = []
    for name in ('engine.zip', 'worlds/mm6.zip', 'worlds/mm7.zip', 'worlds/mm8.zip', 'worlds/mmmerge.zip'):
        with zipfile.ZipFile(REFERENCE / name) as original, zipfile.ZipFile(OUTPUT / name) as built:
            assert built.testzip() is None
            expected = {n for n in original.namelist() if not n.endswith('/')}
            assert set(built.namelist()) == expected
            for filename in sorted(expected):
                before, after = original.read(filename), built.read(filename)
                if filename.lower().endswith('.ogv'):
                    if before == after:
                        video_exact += 1
                    else:
                        video_changed += 1
                    assert after[:4] == b'OggS'
                else:
                    assert before == after, name + ':' + filename
                    nonvideo += 1
            comparisons.append({'archive': name, 'files': len(expected), 'bytes': (OUTPUT / name).stat().st_size,
                                'sha256': sha(OUTPUT / name), 'crc': 'PASS', 'names': 'MATCH'})
        print('Independent comparison passed: ' + name, flush=True)
    assert nonvideo == 53216 and video_exact + video_changed == 529
    for name in ('builder_backend.py', 'source_assets.py', 'asset_delta.py', 'openyamm_asset_probe.py'):
        assert (HERE / name).read_bytes() == (PORTABLE / '_internal' / name).read_bytes(), name
    for p in (HERE / 'data').iterdir():
        if p.is_file():
            assert p.read_bytes() == (PORTABLE / '_internal/data' / p.name).read_bytes(), p.name
    validation = {'builder_version': '0.3.0', 'game_data_target': '01.51',
                  'complete_portable_end_to_end': 'PASS', 'seconds': report['seconds'],
                  'verified_input_containers_and_loose_files': len(report['inputs']),
                  'nonvideo_files_identical_to_prepared_0151': nonvideo,
                  'video_files_identical_to_prepared_0151': video_exact,
                  'video_files_reencoded_differently': video_changed,
                  'archives': comparisons, 'originals_modified': False,
                  'source_isolation': 'Bundled Python ._pth; no global Python or FFmpeg dependency',
                  'safety_tests': '9 behavioral tests PASS; corrupted/missing/overlapping inputs rejected',
                  'gui': 'Compiled, constructed and rendered with bounds checked; not interactive click-tested',
                  'vita': 'No direct device writes. New generated output not hardware-playtested.',
                  'reconstruction': report['reconstruction']}
    directory = PORTABLE / 'validation'
    directory.mkdir(exist_ok=True)
    (directory / 'BUILD-VALIDATION.json').write_text(json.dumps(validation, indent=2), encoding='utf-8')
    (HERE / 'delivery-validation.json').write_text(json.dumps(validation, indent=2), encoding='utf-8')
    for name in ('final-tests.log', 'builder-preview.png'):
        (directory / name).write_bytes((HERE / name).read_bytes())
    for name in ('UiLayoutTest.cs', 'verify_delivery.py'):
        (PORTABLE / 'source' / name).write_bytes((HERE / name).read_bytes())
    # Metadata is deliberately relative; don't package personal absolute input
    # folders, raw captures, saves or temporary conversion artifacts.
    files = [{'path': p.relative_to(PORTABLE).as_posix(), 'bytes': p.stat().st_size, 'sha256': sha(p)}
             for p in sorted(PORTABLE.rglob('*')) if p.is_file() and p.name != 'PACKAGE-FILES.json']
    (PORTABLE / 'PACKAGE-FILES.json').write_text(json.dumps(files, indent=2), encoding='utf-8')
    with zipfile.ZipFile(OUT_ZIP, 'w', zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=True) as z:
        for path in sorted(PORTABLE.rglob('*')):
            if path.is_file():
                z.write(path, PORTABLE.name + '/' + path.relative_to(PORTABLE).as_posix())
    with zipfile.ZipFile(OUT_ZIP) as z:
        assert z.testzip() is None
        assert len(z.namelist()) == len(set(z.namelist()))
        assert not any(n.lower().endswith(('.lod', '.snd', '.vid', '.oysav')) or '/saves/' in n for n in z.namelist())
    package = {'path': OUT_ZIP.name, 'bytes': OUT_ZIP.stat().st_size, 'sha256': sha(OUT_ZIP)}
    (HERE / 'package-result.json').write_text(json.dumps(package, indent=2), encoding='utf-8')
    print(json.dumps({'validation': validation, 'package': package}, indent=2), flush=True)

if __name__ == '__main__':
    main()
