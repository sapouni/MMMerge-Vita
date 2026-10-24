"""Freeze the final preflight input closure and exact-output resource manifest."""
from pathlib import Path
import csv
import hashlib
import json
import shutil

HERE = Path(__file__).resolve().parent
DATA = HERE / 'data'
PORTABLE = HERE / 'portable/MMMerge Vita Asset Builder'

def read(name):
    with (DATA / name).open(encoding='utf-8-sig', newline='') as f:
        return list(csv.DictReader(f))

def main():
    inputs = {(r['game'], r['path'].lower()): r for r in read('supported-inputs.csv')}
    for name in ('recipes.csv', 'patches.csv', 'videos.csv'):
        for row in read(name):
            key = row['source_game'], row['source_container'].lower()
            if key not in inputs:
                path = Path('J:/GOG_GAMES/Might and Magic ' + key[0][-1]) / row['source_container']
                inputs[key] = {'game': key[0], 'path': row['source_container'], 'size': str(path.stat().st_size),
                               'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
    with (DATA / 'supported-inputs.csv').open('w', encoding='utf-8', newline='') as f:
        w = csv.DictWriter(f, fieldnames=['game', 'path', 'size', 'sha256'])
        w.writeheader()
        w.writerows(sorted(inputs.values(), key=lambda r: (r['game'], r['path'])))
    summary = json.loads((DATA / 'resource-summary.json').read_text())
    summary['source_input_files'] = len(inputs)
    summary['direct_output_bytes'] = sum(int(r['output_size']) for r in read('recipes.csv'))
    summary['patched_output_bytes'] = sum(int(r['output_size']) for r in read('patches.csv'))
    summary['patched_bytes_copied_from_originals'] = sum(int(r['source_bytes_copied']) for r in read('patches.csv'))
    summary['locally_transcoded_reference_bytes'] = sum(int(r['output_size_reference']) for r in read('videos.csv'))
    summary['percent_definition'] = 'Reference output bytes belonging to direct extraction, locally source-patched assets, or original video conversions; patches also supply changed bytes.'
    (DATA / 'resource-summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in DATA.iterdir() if p.is_file() and p.name != 'resources.sha256.json'}
    (DATA / 'resources.sha256.json').write_text(json.dumps(hashes, indent=2), encoding='utf-8')
    for p in DATA.iterdir():
        if p.is_file():
            shutil.copyfile(p, PORTABLE / '_internal/data' / p.name)
    for name in ('CoreLauncher.cs', 'BuilderGui.cs', 'prepare_resources.py', 'refine_map_patches.py', 'finalize_resources.py', 'test_builder.py', 'package_windows.py'):
        shutil.copyfile(HERE / name, PORTABLE / 'source' / name)
    print(json.dumps(summary, indent=2))

if __name__ == '__main__':
    main()
