"""Portable, transactional MM6/7/8 -> complete MMMerge Vita runtime builder."""
from __future__ import annotations
from pathlib import Path
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
import argparse
import csv
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zipfile
import asset_delta
from source_assets import Sources, contained, VID_ENTRY

APP_VERSION = '0.3.0'
GAME_VERSION = '01.51'
ARCHIVES = ('engine.zip', 'mm6.zip', 'mm7.zip', 'mm8.zip', 'mmmerge.zip')

def resource_root():
    return Path(getattr(sys, '_MEIPASS', Path(__file__).resolve().parent))

def data_root():
    return resource_root() / 'data'

def sha(value):
    return hashlib.sha256(value).hexdigest()

def sha_file(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for b in iter(lambda: f.read(1024 * 1024), b''):
            h.update(b)
    return h.hexdigest()

def read(name):
    with (data_root() / name).open(encoding='utf-8-sig', newline='') as f:
        return list(csv.DictReader(f))

def emit(message):
    print(message, flush=True)

def ffmpeg_path():
    if getattr(sys, 'frozen', False):
        return resource_root() / 'ffmpeg/ffmpeg.exe'
    override = os.environ.get('MMMERGE_FFMPEG')
    return Path(override) if override else resource_root() / 'ffmpeg/ffmpeg.exe'

def verify_resources():
    hashes = json.loads((data_root() / 'resources.sha256.json').read_text(encoding='utf-8'))
    for name, expected in hashes.items():
        path = contained(data_root(), name)
        if not path.is_file() or sha_file(path) != expected:
            raise ValueError('Builder resource damaged or missing: ' + name)
    for name in ('project-data.zip', 'patches.zip'):
        with zipfile.ZipFile(data_root() / name) as z:
            if z.testzip() is not None:
                raise ValueError('Builder archive is corrupt: ' + name)
    ffmpeg = ffmpeg_path()
    result = subprocess.run([str(ffmpeg), '-version'], capture_output=True,
                            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    if result.returncode or b'ffmpeg version' not in result.stdout.lower():
        raise ValueError('Bundled video converter failed to start')

def validate_folders(mm6, mm7, mm8, output_value):
    roots = {g: Path(v).expanduser().resolve() for g, v in (('mm6', mm6), ('mm7', mm7), ('mm8', mm8))}
    output = Path(output_value).expanduser().resolve()
    for game, root in roots.items():
        if not root.is_dir():
            raise ValueError(f'{game.upper()} installation folder does not exist: {root}')
        if output.is_relative_to(root) or root.is_relative_to(output):
            raise ValueError('Output must not overlap an original game installation')
    if len(set(roots.values())) != 3:
        raise ValueError('Choose the three different game installation folders')
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise ValueError('Choose a new or empty output folder; existing files are never overwritten')
    return roots, output

def verify_inputs(roots):
    rows = read('supported-inputs.csv')
    verified = []
    for i, row in enumerate(rows, 1):
        path = contained(roots[row['game']], row['path'])
        if not path.is_file():
            raise ValueError(f"Missing {row['game'].upper()} input: {row['path']}. Select the installed game folder, not its installer or Data subfolder.")
        if path.stat().st_size != int(row['size']) or sha_file(path) != row['sha256']:
            raise ValueError(f"Unsupported or modified {row['game'].upper()} input: {row['path']}. This version supports the verified English GOG files only; no output has been published.")
        verified.append({'game': row['game'], 'path': row['path'], 'size': int(row['size']), 'sha256': row['sha256']})
        emit(f'Inputs verified {i}/{len(rows)}: {row["game"]}/{row["path"]}')
    return verified

def write_verified(stage, path, value, size, digest):
    if len(value) != int(size) or sha(value) != digest:
        raise ValueError('Reconstructed file does not match the game data: ' + path)
    target = contained(stage, path)
    if target.exists():
        raise ValueError('Duplicate output in builder plan: ' + path)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(value)

def extract_originals(roots, stage):
    sources = Sources(roots)
    rows = sorted(read('recipes.csv'), key=lambda r: (r['source_game'], r['source_container'], r['source_entry'], r['transform']))
    previous = None
    value = b''
    for i, row in enumerate(rows, 1):
        key = tuple(row[k] for k in ('source_game', 'source_kind', 'source_container', 'source_entry', 'transform', 'source_representation_sha256'))
        if key != previous:
            value = sources.read(row)
            if sha(value) != row['source_representation_sha256']:
                raise ValueError('Original asset verification failed: ' + str(key))
            previous = key
        write_verified(stage, row['output_path'], value, row['output_size'], row['output_sha256'])
        if i % 1000 == 0 or i == len(rows):
            emit(f'Original assets {i}/{len(rows)}')
    patches = sorted(read('patches.csv'), key=lambda r: (r['source_game'], r['source_container'], r['source_entry'], r['transform']))
    with zipfile.ZipFile(data_root() / 'patches.zip') as z:
        if set(z.namelist()) != {r['patch'] for r in patches}:
            raise ValueError('Patch resource list mismatch')
        for i, row in enumerate(patches, 1):
            value = sources.read(row)
            if len(value) != int(row['source_size']) or sha(value) != row['source_sha256']:
                raise ValueError('Patch requires different original asset: ' + row['output_path'])
            patch = z.read(row['patch'])
            if len(patch) != int(row['patch_size']) or sha(patch) != row['patch_sha256']:
                raise ValueError('Damaged project patch: ' + row['patch'])
            result = asset_delta.apply(value, patch)
            write_verified(stage, row['output_path'], result, row['output_size'], row['output_sha256'])
            if i % 500 == 0 or i == len(patches):
                emit(f'Local project transforms {i}/{len(patches)}')
    sources.containers.clear()

def add_project_data(stage):
    rows = read('project-data.csv')
    with zipfile.ZipFile(data_root() / 'project-data.zip') as z:
        if set(z.namelist()) != {r['asset_path'] for r in rows}:
            raise ValueError('Supplemental resource list mismatch')
        for row in rows:
            write_verified(stage, row['asset_path'], z.read(row['asset_path']), row['size'], row['sha256'])
    emit(f'Supplemental project files verified: {len(rows)}')

def vid_index(path):
    result = {}
    total = path.stat().st_size
    with path.open('rb') as f:
        count = struct.unpack('<I', f.read(4))[0]
        if count > 100000:
            raise ValueError('Invalid original VID directory')
        rows = [VID_ENTRY.unpack(f.read(VID_ENTRY.size)) for _ in range(count)]
    for i, (name, offset) in enumerate(rows):
        end = rows[i + 1][1] if i + 1 < count else total
        if offset < 4 + count * VID_ENTRY.size or offset > end or end > total:
            raise ValueError('Invalid original VID offset')
        result[name.split(b'\0', 1)[0].decode('latin1').lower()] = offset, end - offset
    return result

def transcode_videos(roots, stage, work, workers):
    grouped = defaultdict(list)
    for row in read('videos.csv'):
        grouped[(row['source_game'], row['source_container'], row['source_entry'], row['source_sha256'])].append(row)
    indices = {}
    for game, container, _, _ in grouped:
        path = contained(roots[game], container)
        indices[(game, container)] = vid_index(path)
    jobs = list(sorted(grouped.items()))
    def convert(index, key, rows):
        game, container, entry, digest = key
        offset, size = indices[(game, container)][entry.lower()]
        with contained(roots[game], container).open('rb') as f:
            f.seek(offset)
            raw = f.read(size)
        if size != int(rows[0]['source_size']) or sha(raw) != digest:
            raise ValueError('Original video changed: ' + str(key))
        input_path, encoded = work / f'video-{index}.smk', work / f'video-{index}.ogv'
        input_path.write_bytes(raw)
        del raw
        command = [str(ffmpeg_path()), '-hide_banner', '-loglevel', 'error', '-nostdin', '-y',
                   '-threads', '1', '-i', str(input_path), '-map', '0:v:0', '-map', '0:a:0?',
                   '-vf', 'format=yuv420p', '-c:v', 'libtheora', '-q:v', '7', '-threads', '1',
                   '-c:a', 'libvorbis', '-q:a', '3', str(encoded)]
        result = subprocess.run(command, capture_output=True, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        if result.returncode or not encoded.is_file() or encoded.stat().st_size < 64:
            raise RuntimeError('Video conversion failed: ' + entry + ' ' + result.stderr.decode('utf-8', 'replace')[-2000:])
        with encoded.open('rb') as f:
            if f.read(4) != b'OggS':
                raise ValueError('Invalid converted video: ' + entry)
        output_hash = sha_file(encoded)
        records = []
        for row in rows:
            target = contained(stage, row['output_path'])
            if target.exists():
                raise ValueError('Duplicate video output: ' + row['output_path'])
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(encoded, target)
            records.append({'path': row['output_path'], 'size': target.stat().st_size,
                            'sha256': output_hash, 'source_sha256': digest})
        input_path.unlink()
        encoded.unlink()
        return records
    results = []
    # A few independent FFmpeg jobs; game logic and game binaries are unchanged.
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = [pool.submit(convert, i, key, rows) for i, (key, rows) in enumerate(jobs)]
        for i, future in enumerate(as_completed(futures), 1):
            results.extend(future.result())
            if i == 1 or i % 10 == 0 or i == len(jobs):
                emit(f'Original video clips converted {i}/{len(jobs)}')
    return sorted(results, key=lambda r: r['path'])

def verify_stage(stage, video_results):
    runtime = read('runtime.csv')
    videos = {r['path']: r for r in video_results}
    for i, row in enumerate(runtime, 1):
        path = contained(stage, row['asset_path'])
        expected = videos.get(row['asset_path'], row)
        if not path.is_file() or path.stat().st_size != int(expected['size']) or sha_file(path) != expected['sha256']:
            raise ValueError('Final validation failed: ' + row['asset_path'])
        if i % 5000 == 0:
            emit(f'Final files verified {i}/{len(runtime)}')
    actual = {p.relative_to(stage).as_posix() for p in stage.rglob('*') if p.is_file()}
    if actual != {r['asset_path'] for r in runtime}:
        raise ValueError('Output file set differs from the complete game manifest')
    return runtime

def create_archives(stage, output, runtime):
    records = []
    for name in ARCHIVES:
        relative = name if name == 'engine.zip' else 'worlds/' + name
        target = output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        rows = sorted((r for r in runtime if r['runtime_archive'] == name), key=lambda r: r['archive_path'])
        with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=True) as z:
            for row in rows:
                info = zipfile.ZipInfo(row['archive_path'], date_time=(2020, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o644 << 16
                z.writestr(info, contained(stage, row['asset_path']).read_bytes(), compresslevel=6)
        with zipfile.ZipFile(target) as z:
            if z.testzip() is not None or len(z.namelist()) != len(rows) or len(set(z.namelist())) != len(rows):
                raise ValueError('Output archive check failed: ' + name)
        records.append({'path': relative, 'files': len(rows), 'size': target.stat().st_size, 'sha256': sha_file(target)})
        emit('Archive ready and verified: ' + relative)
    return records

INSTALL = '''Generated data for MMMerge Vita 01.51
Copy engine.zip and the worlds folder to ux0:data/openyamm/assets/.
Do not unpack these five archives. Install the separately supplied game VPK.
Do not copy the report or replace/delete your saves or personal settings.
The exact same data is used on physical Vita and Vita3K.
This output contains no game executable or Vita kernel plugin.
'''

def build(mm6, mm7, mm8, output_value, workers=2):
    started = time.time()
    if workers not in range(1, 5):
        raise ValueError('Video workers must be between 1 and 4')
    roots, output = validate_folders(mm6, mm7, mm8, output_value)
    emit('Checking bundled recipes, patches and video converter')
    verify_resources()
    emit('Verifying supported original English GOG inputs (read only)')
    inputs = verify_inputs(roots)
    output.parent.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(output.parent).free < 8 * 1024**3:
        raise ValueError('Allow at least 8 GiB free on the output drive for temporary files and final archives')
    lock = output.parent / ('.' + output.name + '.mmmerge-build.lock')
    fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    os.close(fd)
    try:
        # Recheck after acquiring the per-output lock. All generation is outside
        # the user's output folder; only a verified complete tree is published.
        validate_folders(mm6, mm7, mm8, output_value)
        with tempfile.TemporaryDirectory(prefix='mmmerge-build-', dir=output.parent) as temporary:
            work = Path(temporary)
            stage, publish = work / 'stage', work / 'publish'
            stage.mkdir()
            publish.mkdir()
            emit('Extracting original files and applying verified local patches')
            extract_originals(roots, stage)
            emit('Adding merged scripts, tables, scenes and supplemental media')
            add_project_data(stage)
            emit('Converting original game videos; this can take several minutes')
            videos = transcode_videos(roots, stage, work, workers)
            emit('Checking every file against the complete game manifest')
            runtime = verify_stage(stage, videos)
            archives = create_archives(stage, publish, runtime)
            summary = json.loads((data_root() / 'resource-summary.json').read_text(encoding='utf-8'))
            report = {'builder_version': APP_VERSION, 'game_data_version': GAME_VERSION, 'status': 'complete',
                      'seconds': round(time.time() - started, 2), 'inputs': inputs,
                      'source_folders': {g: str(p) for g, p in roots.items()},
                      'runtime_files': len(runtime), 'archives': archives, 'videos': videos,
                      'reconstruction': summary, 'nonvideo_comparison': 'Exact 01.51 runtime file hashes',
                      'video_comparison': 'Locally re-encoded from verified original clips; encoded hashes may differ',
                      'hardware_validation': 'Builder output has not yet been played on a physical Vita'}
            (publish / 'MMMerge-build-report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
            (publish / 'INSTALL-DATA.txt').write_text(INSTALL, encoding='utf-8')
            if output.exists():
                output.rmdir()  # Fails safely if anything appeared meanwhile.
            os.replace(publish, output)
    finally:
        lock.unlink(missing_ok=True)
    emit('COMPLETE: ' + str(output))
    return output

def main():
    parser = argparse.ArgumentParser(description='Build complete MMMerge Vita data from installed English GOG MM6, MM7 and MM8')
    for game in ('mm6', 'mm7', 'mm8'):
        parser.add_argument('--' + game)
    parser.add_argument('--output')
    parser.add_argument('--video-workers', type=int, default=2)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    try:
        if args.self_test:
            verify_resources()
            runtime = read('runtime.csv')
            generated = [r['output_path'] for name in ('recipes.csv', 'patches.csv', 'videos.csv') for r in read(name)]
            generated.extend(r['asset_path'] for r in read('project-data.csv'))
            if len(generated) != len(set(generated)) or set(generated) != {r['asset_path'] for r in runtime}:
                raise ValueError('Builder plan is incomplete or has duplicate outputs')
            emit(f'SELF-TEST OK: {len(runtime)} runtime files, complete disjoint reconstruction plan, resources and FFmpeg verified')
            return 0
        if not all((args.mm6, args.mm7, args.mm8, args.output)):
            parser.error('--mm6, --mm7, --mm8 and --output are required')
        build(args.mm6, args.mm7, args.mm8, args.output, args.video_workers)
        return 0
    except Exception as error:
        emit(f'ERROR {type(error).__name__}: {error}')
        return 2

if __name__ == '__main__':
    raise SystemExit(main())
