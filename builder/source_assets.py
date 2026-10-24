"""Read-only grouped access to supported original-game representations."""
from pathlib import Path
from collections import OrderedDict
import zlib
from openyamm_asset_probe import (
    iter_lod_records, decode_lod_record, decode_lod_image_as_bmp,
    decode_lod_sprite_as_bmp, find_game_palettes, SND_ENTRY, VID_ENTRY,
)
import struct
import hashlib

def contained(root: Path, relative: str) -> Path:
    if '\\' in relative or ':' in relative:
        raise ValueError('invalid relative asset path: ' + relative)
    path = (root / relative).resolve()
    if not path.is_relative_to(root.resolve()):
        raise ValueError('path escapes root: ' + relative)
    return path

class Sources:
    def __init__(self, roots):
        self.roots = roots
        self.containers = OrderedDict()
        warnings = []
        self.palettes = {g: find_game_palettes(p, warnings, g) for g, p in roots.items()}
        if warnings:
            raise ValueError('cannot read original palettes: ' + str(warnings))

    def records(self, game, kind, container):
        key = game, kind, container
        if key in self.containers:
            self.containers.move_to_end(key)
            return self.containers[key]
        path = contained(self.roots[game], container)
        if kind == 'lod':
            result = {}
            for name, _, raw in iter_lod_records(path):
                result.setdefault(name, []).append((raw, 0))
        elif kind == 'snd':
            result = {}
            with path.open('rb') as f:
                count = struct.unpack('<I', f.read(4))[0]
                if count > 100000:
                    raise ValueError('invalid SND directory')
                entries = [SND_ENTRY.unpack(f.read(SND_ENTRY.size)) for _ in range(count)]
                for name, offset, size, decoded in entries:
                    if offset + size > path.stat().st_size:
                        raise ValueError('SND entry outside archive')
                    f.seek(offset)
                    result.setdefault(name.split(b'\0', 1)[0].decode('latin1'), []).append((f.read(size), decoded))
        elif kind == 'vid':
            result = {}
            with path.open('rb') as f:
                count = struct.unpack('<I', f.read(4))[0]
                if count > 100000:
                    raise ValueError('invalid VID directory')
                entries = [VID_ENTRY.unpack(f.read(VID_ENTRY.size)) for _ in range(count)]
                total = path.stat().st_size
                for i, (name, offset) in enumerate(entries):
                    end = entries[i + 1][1] if i + 1 < count else total
                    if offset > end or end > total:
                        raise ValueError('VID entry outside archive')
                    f.seek(offset)
                    result.setdefault(name.split(b'\0', 1)[0].decode('latin1'), []).append((f.read(end - offset), 0))
        else:
            raise ValueError('unsupported source container: ' + kind)
        self.containers[key] = result
        # Bounded number of original archives resident at once.
        while len(self.containers) > 2:
            self.containers.popitem(last=False)
        return result

    def read(self, row):
        game, kind = row['source_game'], row['source_kind']
        container, entry, rep = row['source_container'], row['source_entry'], row['transform']
        if kind == 'loose':
            if rep != 'raw':
                raise ValueError('unsupported loose-file transform')
            return contained(self.roots[game], container).read_bytes()
        expected = row.get('source_sha256', row.get('source_representation_sha256'))
        records = self.records(game, kind, container)[entry]
        for raw, decoded_size in reversed(records):
            value = self.decode(game, kind, rep, raw, decoded_size)
            if expected is None or hashlib.sha256(value).hexdigest() == expected:
                return value
        raise ValueError('No original entry representation matches the required hash: ' + str((game, container, entry, rep)))

    def decode(self, game, kind, rep, raw, decoded_size):
        if rep == 'raw':
            return raw
        if kind == 'snd' and rep == 'decoded':
            return zlib.decompress(raw) if decoded_size and decoded_size != len(raw) else raw
        if rep.startswith('bmp-image'):
            value = decode_lod_image_as_bmp(raw, horizontal_flip=rep.endswith('-hflip'))
        elif rep.startswith('bmp-sprite'):
            palette_game = rep.split('-pal-')[1][:3] if '-pal-' in rep else game
            value = decode_lod_sprite_as_bmp(raw, self.palettes[palette_game], horizontal_flip=rep.endswith('-hflip'))
        else:
            value = dict(decode_lod_record(raw)).get(rep)
        if value is None:
            raise ValueError('cannot decode original representation: ' + str((game, rep)))
        return value
