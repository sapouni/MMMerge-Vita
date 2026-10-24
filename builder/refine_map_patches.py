"""Move original-based merged geometry out of the supplement into source deltas."""
from pathlib import Path
from collections import defaultdict
import csv
import hashlib
import json
import shutil
import zipfile
import zlib
import asset_delta
from source_assets import Sources

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OLD = HERE / 'data'
NEW = HERE / 'refined-data-final'
REPORTS = ROOT / 'outputs/gog-asset-provenance'

def read(path):
    with path.open(encoding='utf-8-sig', newline='') as f:
        return list(csv.DictReader(f))

def write(name, rows):
    with (NEW / name).open('w', encoding='utf-8', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)

def sha(value):
    return hashlib.sha256(value).hexdigest()

def main():
    if NEW.exists():
        raise FileExistsError(NEW)
    NEW.mkdir()
    roots = {g: Path('J:/GOG_GAMES/Might and Magic ' + g[-1]) for g in ('mm6', 'mm7', 'mm8')}
    sources = Sources(roots)
    native = {r['output_geometry_path']: r for r in read(REPORTS / 'GOG-map-source-manifest.csv')}
    inventory = defaultdict(list)
    for row in read(REPORTS / 'GOG-source-entry-inventory.csv'):
        if row['container_kind'] == 'lod' and row['representation'] == 'decoded' and Path(row['entry_name']).suffix in ('.blv', '.odm'):
            inventory[(row['game'], Path(row['entry_name']).name.lower())].append(row)
    old_rows = read(OLD / 'project-data.csv')
    patches = read(OLD / 'patches.csv')
    remaining, moved = [], []
    with zipfile.ZipFile(OLD / 'project-data.zip') as original, zipfile.ZipFile(NEW / 'project-data.zip', 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as extra, zipfile.ZipFile(NEW / 'patches.zip', 'w', zipfile.ZIP_STORED) as out, zipfile.ZipFile(OLD / 'patches.zip') as old_patches:
        for info in old_patches.infolist():
            out.writestr(info, old_patches.read(info.filename))
        for row in old_rows:
            path = row['asset_path']
            target = original.read(path)
            source_row = None
            if path in native:
                r = native[path]
                for source in inventory[(r['world'], Path(r['source_geometry_entry']).name.lower())]:
                    if source['sha256'] == r['source_geometry_sha256']:
                        source_row = source
                        break
            elif path.startswith('worlds/mm8/maps/') and Path(path).suffix in ('.blv', '.odm'):
                rows = inventory[('mm8', Path(path).name.lower())]
                if rows:
                    source_row = rows[0]
            if source_row:
                source_spec = {'source_game': source_row['game'], 'source_kind': 'lod', 'source_container': source_row['container_path'],
                               'source_entry': source_row['entry_name'], 'transform': 'decoded', 'source_sha256': source_row['sha256']}
                value = sources.read(source_spec)
                packed, copied = asset_delta.create(value, target)
                # MM6/7 -> merged MM8-layout conversion expands/reorders fields.
                # Require an actual owned-map contribution. Field-layout
                # conversion can make a correct map patch compress almost as
                # large as the full map, so report both sizes rather than
                # substituting the original geometry or shipping the full map.
                ratio = copied / len(target)
                full_size = len(zlib.compress(target, 9))
                if copied > len(target) * .10:
                    assert asset_delta.apply(value, packed) == target
                    patch_name = f'{len(patches):05d}.mmpd'
                    out.writestr(patch_name, packed)
                    patches.append({'output_path': path, 'output_size': str(len(target)), 'output_sha256': row['sha256'],
                        **source_spec, 'source_size': source_row['size'], 'patch': patch_name, 'patch_sha256': sha(packed),
                        'patch_size': str(len(packed)), 'source_bytes_copied': str(copied)})
                    moved.append({'path': path, 'output_bytes': len(target), 'source_copy_percent': round(ratio * 100, 2),
                                  'patch_bytes': len(packed), 'full_compressed_bytes': full_size})
                    print(f'Map patch {path}: copies {ratio:.1%}, {len(packed)}/{full_size} compressed bytes', flush=True)
                    continue
                print(f'Map remains supplemental {path}: copies {ratio:.1%}, {len(packed)}/{full_size} compressed bytes', flush=True)
            extra.writestr(path, target)
            remaining.append(row)
    for p in OLD.iterdir():
        if p.name not in ('project-data.zip', 'project-data.csv', 'patches.zip', 'patches.csv', 'resource-summary.json', 'resources.sha256.json', 'resource-inventory.csv'):
            shutil.copyfile(p, NEW / p.name)
    write('patches.csv', patches)
    write('project-data.csv', remaining)
    # Newly source-linked geometry can introduce a source container that the
    # former supplement-only plan never needed to preflight (MM6 GAMES.LOD).
    inputs = {(r['game'], r['path'].lower()): r for r in read(OLD / 'supported-inputs.csv')}
    for row in patches:
        key = row['source_game'], row['source_container'].lower()
        if key not in inputs:
            path = roots[key[0]] / row['source_container']
            inputs[key] = {'game': key[0], 'path': row['source_container'], 'size': str(path.stat().st_size), 'sha256': sha(path.read_bytes())}
    write('supported-inputs.csv', sorted(inputs.values(), key=lambda r: (r['game'], r['path'])))
    runtime = read(OLD / 'runtime.csv')
    summary = json.loads((OLD / 'resource-summary.json').read_text())
    summary.update(source_patch_files=len(patches), supplemental_files=len(remaining),
                   supplemental_uncompressed_bytes=sum(int(r['size']) for r in remaining),
                   patch_bytes=sum(int(r['patch_size']) for r in patches), source_input_files=len(inputs))
    summary['local_reference_bytes'] = summary['runtime_bytes'] - summary['supplemental_uncompressed_bytes']
    summary['local_reference_percent'] = round(100 * summary['local_reference_bytes'] / summary['runtime_bytes'], 2)
    (NEW / 'resource-summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    write('map-patch-evidence.csv', moved)
    write('resource-inventory.csv', [{'asset_path': r['output_path'], 'method': 'local-source-patch', 'size': r['output_size']} for r in patches] +
          [{'asset_path': r['asset_path'], 'method': 'supplemental-project-data', 'size': r['size']} for r in remaining])
    hashes = {p.name: sha(p.read_bytes()) for p in NEW.iterdir() if p.is_file()}
    (NEW / 'resources.sha256.json').write_text(json.dumps(hashes, indent=2), encoding='utf-8')
    print(json.dumps(summary, indent=2), flush=True)

if __name__ == '__main__':
    main()
