"""Behavioral safety and format checks for the new converter, without a Vita."""
from pathlib import Path
import csv
import hashlib
import json
import os
import random
import struct
import tempfile
import unittest
import zlib
import asset_delta
import builder_backend as b
from source_assets import contained, Sources

HERE = Path(__file__).resolve().parent

class DeltaTests(unittest.TestCase):
    def test_roundtrips(self):
        rng = random.Random(341)
        for count in (0, 1, 15, 16, 17, 24, 25, 100, 2048, 65000):
            for mode in range(8):
                source = rng.randbytes(count) if mode % 2 else b'\0' * count
                target = bytearray(source)
                if target:
                    start = rng.randrange(len(target))
                    target[start:start + 7] = b'changed'
                if mode & 2:
                    target[:0] = b'inserted header'
                if mode & 4:
                    target.extend(source[::-1][:2048])
                packed, copied = asset_delta.create(source, bytes(target))
                self.assertEqual(asset_delta.apply(source, packed), bytes(target))
                self.assertLessEqual(copied, len(target))
    def test_reject_bounds(self):
        header = asset_delta.MAGIC + struct.pack('<II', 4, 4)
        for tail in (b'X', b'C', b'C' + struct.pack('<II', 3, 5), b'L' + struct.pack('<I', 100), b'L' + struct.pack('<I', 5) + b'12345'):
            with self.assertRaises(ValueError):
                asset_delta.apply(b'1234', zlib.compress(header + tail))
        patch, _ = asset_delta.create(b'base', b'target')
        with self.assertRaises(ValueError):
            asset_delta.apply(b'wrong size', patch)
        with self.assertRaises(ValueError):
            asset_delta.apply(b'base', patch + b'trailing')

class PlanTests(unittest.TestCase):
    def test_complete_disjoint_plan(self):
        paths = [r['output_path'] for n in ('recipes.csv', 'patches.csv', 'videos.csv') for r in b.read(n)]
        paths += [r['asset_path'] for r in b.read('project-data.csv')]
        self.assertEqual(len(paths), 53745)
        self.assertEqual(len(set(paths)), 53745)
        self.assertEqual(set(paths), {r['asset_path'] for r in b.read('runtime.csv')})
    def test_every_source_is_preflight_verified(self):
        inputs = {(r['game'], r['path'].lower()) for r in b.read('supported-inputs.csv')}
        for name in ('recipes.csv', 'patches.csv', 'videos.csv'):
            for row in b.read(name):
                self.assertIn((row['source_game'], row['source_container'].lower()), inputs)
    def test_path_boundaries(self):
        with tempfile.TemporaryDirectory(dir=HERE) as temp:
            root = Path(temp)
            for bad in ('../escape', '/absolute', 'C:/escape', 'folder\\escape'):
                with self.assertRaises(ValueError):
                    contained(root, bad)
    def test_output_protection(self):
        with tempfile.TemporaryDirectory(dir=HERE) as temp:
            root = Path(temp)
            games = [root / ('game' + str(n)) for n in range(3)]
            for p in games:
                p.mkdir()
            output = root / 'output'
            output.mkdir()
            marker = output / 'keep.txt'
            marker.write_text('untouched')
            with self.assertRaises(ValueError):
                b.validate_folders(*map(str, games), str(output))
            self.assertEqual(marker.read_text(), 'untouched')
            for bad in (games[0] / 'output', root, games[1]):
                with self.assertRaises(ValueError):
                    b.validate_folders(*map(str, games), str(bad))
            with self.assertRaises(ValueError):
                b.validate_folders(str(games[0]), str(games[0]), str(games[2]), str(root / 'new'))
    def test_missing_input_leaves_output_absent(self):
        with tempfile.TemporaryDirectory(dir=HERE) as temp:
            root = Path(temp)
            games = [root / ('game' + str(n)) for n in range(3)]
            for p in games:
                p.mkdir()
            output = root / 'output'
            with self.assertRaises(ValueError):
                b.build(*map(str, games), str(output))
            self.assertFalse(output.exists())
            self.assertFalse(list(root.glob('mmmerge-build-*')))
    def test_corrupt_input_rejected(self):
        with tempfile.TemporaryDirectory(dir=HERE) as temp:
            root = Path(temp)
            rows = b.read('supported-inputs.csv')
            first = rows[0]
            original = root / first['path']
            original.parent.mkdir(parents=True)
            original.write_bytes(b'wrong input')
            roots = {g: root for g in ('mm6', 'mm7', 'mm8')}
            with self.assertRaisesRegex(ValueError, 'Unsupported or modified'):
                b.verify_inputs(roots)
    def test_duplicate_truncated_name_selects_content(self):
        source = object.__new__(Sources)
        source.roots = {}
        source.palettes = {}
        source.records = lambda *unused: {'collision': [(b'correct', 0), (b'different', 0)]}
        row = {'source_game': 'mm8', 'source_kind': 'lod', 'source_container': 'Data/example.lod',
               'source_entry': 'collision', 'transform': 'raw', 'source_sha256': hashlib.sha256(b'correct').hexdigest()}
        self.assertEqual(source.read(row), b'correct')
        row['source_sha256'] = '0' * 64
        with self.assertRaises(ValueError):
            source.read(row)

if __name__ == '__main__':
    unittest.main(verbosity=2)
