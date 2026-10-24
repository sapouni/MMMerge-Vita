"""Developer-only recipe/patch preparation; no reference tree ships in the tool."""
from pathlib import Path
from collections import Counter
from functools import lru_cache
import csv
import hashlib
import json
import zipfile
from source_assets import Sources
import asset_delta

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
REPORTS = ROOT / 'outputs/gog-asset-provenance'
DATA = HERE / 'data'
ASSETS = ROOT / 'work/vita3k-runner/portable/fs/ux0/data/openyamm/assets'

def read(path):
    with path.open(encoding='utf-8-sig', newline='') as f:
        return list(csv.DictReader(f))

def write(name, rows):
    with (DATA / name).open('w', encoding='utf-8', newline='') as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)

def sha(value):
    return hashlib.sha256(value).hexdigest()

def main():
    DATA.mkdir(exist_ok=True)
    if (DATA / 'runtime.csv').exists():
        raise FileExistsError('resources already prepared; preserve them')
    runtime = read(REPORTS / 'OpenYAMM-runtime-required.csv')
    expected = {r['asset_path']: r for r in runtime}
    direct = [r for r in read(REPORTS / 'GOG-to-OpenYAMM-build-recipe.csv') if r['output_path'] in expected]
    direct_by_path = {r['output_path']: r for r in direct}
    videos = read(REPORTS / 'GOG-video-transcode-manifest.csv')
    video_paths = {r['output_path'] for r in videos}
    semantic = {r['asset_path']: r for r in read(REPORTS / 'OpenYAMM-runtime-image-semantic-matches.csv')}
    names = {r['asset_path']: r for r in read(REPORTS / 'OpenYAMM-runtime-name-remap-candidates.csv')}
    inventory = {}
    for r in read(REPORTS / 'GOG-source-entry-inventory.csv'):
        inventory[(r['game'], r['container_path'].lower(), r['entry_name'].lower(), r['representation'])] = r
    native = {r['output_geometry_path']: r for r in read(REPORTS / 'GOG-map-source-manifest.csv')}
    roots = {g: Path('J:/GOG_GAMES') / ('Might and Magic ' + g[-1]) for g in ('mm6', 'mm7', 'mm8')}
    sources = Sources(roots)
    archives = {n: zipfile.ZipFile(ASSETS / (n if n == 'engine.zip' else 'worlds/' + n)) for n in {r['runtime_archive'] for r in runtime}}

    @lru_cache(maxsize=512)
    def source_value(game, container, entry, rep):
        row = inventory[(game, container.lower(), entry.lower(), rep)]
        value = sources.read({'source_game': game, 'source_kind': row['container_kind'],
                              'source_container': row['container_path'], 'source_entry': row['entry_name'], 'transform': rep})
        assert len(value) == int(row['size']) and sha(value) == row['sha256']
        return value, row

    def candidates(path):
        result = []
        if path in native:
            r = native[path]
            result.append((r['world'], r['source_container'], r['source_geometry_entry'], 'decoded'))
        sem = semantic.get(path, {})
        for key in ('semantic_matches', 'index_matches'):
            for s in sem.get(key, '').split(' | '):
                if s:
                    result.append(tuple(s.split(':')))
        other = []
        for s in names.get(path, {}).get('source_candidates', '').split(' | '):
            if not s:
                continue
            g, c, e, rep, size, digest = s.split(':')
            ext = Path(path).suffix.lower()
            if ext == '.bmp' and not rep.startswith('bmp-'):
                continue
            if ext in ('.blv', '.odm') and not (e.lower().endswith(ext) and rep == 'decoded'):
                continue
            if ext == '.act' and rep != 'raw':
                continue
            preferred = path.split('/')[1] if path.startswith('worlds/mm') else ('mm' + Path(path).name[0] if Path(path).name[0] in '678' else '')
            other.append((g != preferred, abs(int(size) - int(expected[path]['size'])), (g, c, e, rep)))
        result.extend(x[2] for x in sorted(other))
        return list(dict.fromkeys(result))[:10]

    remaining = [r for r in runtime if r['asset_path'] not in direct_by_path and r['asset_path'] not in video_paths]
    # Keep source-container traversal local to avoid rereading LODs for every output.
    choices = {r['asset_path']: candidates(r['asset_path']) for r in remaining}
    remaining.sort(key=lambda r: (choices[r['asset_path']][0][:2] if choices[r['asset_path']] else ('zz', 'zz'), r['asset_path']))
    patches, additions, inventory_out = [], [], []
    with zipfile.ZipFile(DATA / 'project-data.zip', 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as extra, zipfile.ZipFile(DATA / 'patches.zip', 'w', zipfile.ZIP_STORED) as patches_zip:
        for i, row in enumerate(remaining, 1):
            path = row['asset_path']
            if i % 25 == 1:
                print('Preparing ' + str(i) + ': ' + path, flush=True)
            target = archives[row['runtime_archive']].read(row['archive_path'])
            assert len(target) == int(row['size']) and sha(target) == row['sha256'], path
            best = None
            for candidate in choices[path]:
                try:
                    value, source = source_value(*candidate)
                except (KeyError, ValueError):
                    continue
                packed, copied = asset_delta.create(value, target)
                # Never relabel a full replacement as a source-derived patch.
                if copied < len(target) * .60 or len(packed) > len(target) * .60:
                    continue
                if best is None or len(packed) < len(best[0]):
                    best = packed, copied, source
                if copied > len(target) * .97:
                    break
            if best:
                packed, copied, source = best
                source_data, _ = source_value(source['game'], source['container_path'], source['entry_name'], source['representation'])
                assert asset_delta.apply(source_data, packed) == target
                patch_name = f'{len(patches):05d}.mmpd'
                patches_zip.writestr(patch_name, packed)
                patches.append({'output_path': path, 'output_size': len(target), 'output_sha256': row['sha256'],
                    'source_game': source['game'], 'source_kind': source['container_kind'],
                    'source_container': source['container_path'], 'source_entry': source['entry_name'],
                    'transform': source['representation'], 'source_size': source['size'], 'source_sha256': source['sha256'],
                    'patch': patch_name, 'patch_sha256': sha(packed), 'patch_size': len(packed), 'source_bytes_copied': copied})
                disposition = 'local-source-patch'
            else:
                extra.writestr(path, target)
                additions.append({'asset_path': path, 'size': len(target), 'sha256': row['sha256'],
                    'category': 'supplemental-project-data',
                    'provenance': 'Mixed merged tables/scripts/scenes, additional media and unproven residual assets; not claimed original-game-free'})
                disposition = 'supplemental-project-data'
            inventory_out.append({'asset_path': path, 'method': disposition, 'size': len(target)})
            if i % 100 == 0 or i == len(remaining):
                print(f'resources {i}/{len(remaining)}: patches={len(patches)} supplemental={len(additions)}', flush=True)
    for archive in archives.values():
        archive.close()
    write('runtime.csv', runtime)
    write('recipes.csv', direct)
    write('videos.csv', videos)
    write('patches.csv', patches)
    write('project-data.csv', additions)
    write('resource-inventory.csv', inventory_out)
    all_sources = {(r['source_game'], r['source_container']) for r in direct + patches + videos}
    # Sprite conversion palettes are inputs too, even if no output names them.
    for game, root in roots.items():
        for path in root.rglob('*'):
            if path.is_file() and path.name.lower() == 'bitmaps.lod':
                all_sources.add((game, path.relative_to(root).as_posix()))
    inputs = []
    for game, relative in sorted(all_sources):
        path = roots[game] / relative
        h = hashlib.sha256()
        with path.open('rb') as f:
            for block in iter(lambda: f.read(1024 * 1024), b''):
                h.update(block)
        inputs.append({'game': game, 'path': relative, 'size': path.stat().st_size, 'sha256': h.hexdigest()})
    write('supported-inputs.csv', inputs)
    bytes_direct = sum(int(r['output_size']) for r in direct)
    bytes_patched = sum(int(r['output_size']) for r in patches)
    bytes_video = sum(int(r['output_size_reference']) for r in videos)
    total = sum(int(r['size']) for r in runtime)
    summary = {'runtime_files': len(runtime), 'direct_files': len(direct), 'source_patch_files': len(patches),
               'video_files': len(videos), 'supplemental_files': len(additions), 'runtime_bytes': total,
               'local_reference_bytes': bytes_direct + bytes_patched + bytes_video,
               'local_reference_percent': round(100 * (bytes_direct + bytes_patched + bytes_video) / total, 2),
               'supplemental_uncompressed_bytes': sum(int(r['size']) for r in additions),
               'patch_bytes': sum(int(r['patch_size']) for r in patches),
               'source_input_files': len(inputs),
               'provenance_limit': 'Supplemental media is not fully traced; package is not asserted original-game-free.'}
    (DATA / 'resource-summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    hashes = {p.name: sha(p.read_bytes()) for p in DATA.iterdir() if p.is_file()}
    (DATA / 'resources.sha256.json').write_text(json.dumps(hashes, indent=2), encoding='utf-8')
    print(json.dumps(summary, indent=2), flush=True)

if __name__ == '__main__':
    main()
