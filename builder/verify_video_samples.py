"""Check representative original/conversion streams and decode encoded samples."""
from pathlib import Path
from fractions import Fraction
import csv
import json
import subprocess
import tempfile
import zipfile
from builder_backend import vid_index

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
BIN = ROOT / 'work/toolchains/ffmpeg-release-essentials-extracted/ffmpeg-9.0.2-essentials_build/bin'
OUTPUT = HERE / 'portable final verified data'

def probe(path):
    result = subprocess.run([str(BIN / 'ffprobe.exe'), '-v', 'error', '-show_entries',
                            'stream=codec_type,width,height,r_frame_rate,sample_rate,channels,duration:format=duration',
                            '-of', 'json', str(path)], capture_output=True, text=True)
    if result.returncode:
        raise ValueError(result.stderr)
    return json.loads(result.stdout)

def main():
    with (HERE / 'data/videos.csv').open(encoding='utf-8', newline='') as f:
        rows = list(csv.DictReader(f))
    selected = []
    for game in ('mm6', 'mm7', 'mm8'):
        group = [r for r in rows if r['source_game'] == game]
        selected.extend(sorted(group, key=lambda r: -int(r['source_size']))[:2])
        for row in group:
            if row['output_path'] not in {r['output_path'] for r in selected}:
                selected.append(row)
                break
    results = []
    with tempfile.TemporaryDirectory(prefix='video-check-', dir=HERE) as tmp:
        tmp = Path(tmp)
        for row in selected:
            root = Path('J:/GOG_GAMES/Might and Magic ' + row['source_game'][-1])
            path = root / row['source_container']
            offset, count = vid_index(path)[row['source_entry'].lower()]
            with path.open('rb') as f:
                f.seek(offset)
                (tmp / 'original.smk').write_bytes(f.read(count))
            world = row['output_path'].split('/')[1]
            relative = row['output_path'].split('/', 2)[2]
            with zipfile.ZipFile(OUTPUT / 'worlds' / (world + '.zip')) as z:
                (tmp / 'converted.ogv').write_bytes(z.read(relative))
            source, converted = probe(tmp / 'original.smk'), probe(tmp / 'converted.ogv')
            before = next(s for s in source['streams'] if s['codec_type'] == 'video')
            after = next(s for s in converted['streams'] if s['codec_type'] == 'video')
            assert (before['width'], before['height']) == (after['width'], after['height'])
            assert abs(float(Fraction(before['r_frame_rate'])) - float(Fraction(after['r_frame_rate']))) < .01
            duration_before = float(source['format']['duration'])
            duration_after = float(converted['format']['duration'])
            assert abs(duration_after - duration_before) < .5, (row['output_path'], duration_before, duration_after)
            source_audio = [s for s in source['streams'] if s['codec_type'] == 'audio']
            target_audio = [s for s in converted['streams'] if s['codec_type'] == 'audio']
            assert bool(source_audio) == bool(target_audio)
            decode = subprocess.run([str(BIN / 'ffmpeg.exe'), '-v', 'error', '-nostdin', '-i', str(tmp / 'converted.ogv'),
                                     '-t', '3', '-f', 'null', '-'], capture_output=True)
            assert decode.returncode == 0, decode.stderr
            results.append({'path': row['output_path'], 'width': after['width'], 'height': after['height'],
                            'fps': after['r_frame_rate'], 'source_seconds': duration_before,
                            'converted_seconds': duration_after, 'audio_preserved': bool(target_audio), 'decode_3_seconds': 'PASS'})
            print('Video sample verified: ' + row['output_path'], flush=True)
    report = {'status': 'PASS', 'samples': results, 'limit': 'Nine representative clips; not an exhaustive video playthrough or hardware test.'}
    (HERE / 'video-sample-validation.json').write_text(json.dumps(report, indent=2), encoding='utf-8')

if __name__ == '__main__':
    main()
