#!/usr/bin/env python3
"""Read-only provenance probe for building OpenYAMM assets from owned game data.

This is an independent implementation of the documented MM6-MM8 container
layouts.  It never writes to the supplied game directories.  Its purpose is to
identify which OpenYAMM development assets are byte-identical to loose GOG
files or entries in LOD/VID/SND archives, and which assets need a transform,
generator, or OpenYAMM-specific overlay.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import struct
import sys
import time
import zlib
from collections import Counter, defaultdict
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import BinaryIO, Iterable, Iterator


CHUNK = 1024 * 1024
LOD_HEADER = struct.Struct("<4s80s80sIII80s")
LOD_ENTRY_MM6 = struct.Struct("<16sIIIHH")
LOD_ENTRY_MM8 = struct.Struct("<16s15I")
COMPRESSION_HEADER = struct.Struct("<I4sII")
MM6_COMPRESSION_HEADER = struct.Struct("<II")
PSEUDO_IMAGE_HEADER = struct.Struct("<16sIIHHhhhhhhII")
WIDE_PSEUDO_IMAGE_HEADER = struct.Struct("<64sIIHHhhhhhhII")
VID_ENTRY = struct.Struct("<40sI")
SND_ENTRY = struct.Struct("<40sIII")
LOD_SPRITE_HEADER = struct.Struct("<12sI6HI")
LOD_SPRITE_LINE = struct.Struct("<hhI")


@dataclass(frozen=True)
class SourceBlob:
    game: str
    container_kind: str
    container_path: str
    entry_name: str
    representation: str
    size: int
    sha256: str


def c_string(value: bytes) -> str:
    return value.split(b"\0", 1)[0].decode("latin-1", errors="replace")


def sha_stream(stream: BinaryIO, size: int | None = None) -> tuple[str, int]:
    digest = hashlib.sha256()
    total = 0
    remaining = size
    while remaining is None or remaining > 0:
        request = CHUNK if remaining is None else min(CHUNK, remaining)
        data = stream.read(request)
        if not data:
            break
        digest.update(data)
        total += len(data)
        if remaining is not None:
            remaining -= len(data)
    if size is not None and total != size:
        raise ValueError(f"short read: expected {size}, got {total}")
    return digest.hexdigest(), total


def sha_file(path: Path) -> tuple[str, int]:
    with path.open("rb") as stream:
        return sha_stream(stream)


def decode_lod_record(raw: bytes) -> list[tuple[str, bytes]]:
    """Return additional decoded representations for a LOD record."""
    decoded: list[tuple[str, bytes]] = []

    # MM6 GAMES.LOD map records use the older compact wrapper:
    # [compressed byte count][decompressed byte count][zlib stream].
    # The first count excludes this eight-byte header.
    if len(raw) >= MM6_COMPRESSION_HEADER.size:
        compressed_size, decompressed_size = MM6_COMPRESSION_HEADER.unpack_from(raw)
        if compressed_size == len(raw) - MM6_COMPRESSION_HEADER.size and decompressed_size > 0:
            try:
                value = zlib.decompress(raw[MM6_COMPRESSION_HEADER.size:])
            except zlib.error:
                # Two files in the unmodified GOG MM6 archive (d08.blv and
                # cd3.blv) carry a bad zlib checksum even though their DEFLATE
                # payload is intact and expands to the declared size. The
                # original game accepts them. Recover only when the raw stream
                # is complete and its size matches the archive metadata.
                try:
                    payload = raw[MM6_COMPRESSION_HEADER.size:]
                    value = zlib.decompress(payload[2:-4], -zlib.MAX_WBITS)
                except zlib.error:
                    value = b""
            if value:
                if len(value) != decompressed_size:
                    raise ValueError("MM6 LOD decompressed-size mismatch")
                decoded.append(("decoded", value))

    if len(raw) >= COMPRESSION_HEADER.size:
        version, signature, data_size, decompressed_size = COMPRESSION_HEADER.unpack_from(raw)
        if version == 91969 and signature == b"mvii":
            payload_size = len(raw) - COMPRESSION_HEADER.size if data_size == len(raw) else data_size
            end = COMPRESSION_HEADER.size + payload_size
            if 0 <= payload_size <= len(raw) - COMPRESSION_HEADER.size:
                payload = raw[COMPRESSION_HEADER.size:end]
                if decompressed_size:
                    value = zlib.decompress(payload)
                    if len(value) != decompressed_size:
                        raise ValueError("LOD decompressed-size mismatch")
                else:
                    value = payload
                decoded.append(("decoded", value))

    # Some LOD entries are a pseudo-image wrapper around compressed data.
    if len(raw) >= PSEUDO_IMAGE_HEADER.size:
        fields = PSEUDO_IMAGE_HEADER.unpack_from(raw)
        (_name, image_size, data_size, width, height, width_ln2, height_ln2,
         width_minus_1, height_minus_1, palette_id, palette_id_2,
         decompressed_size, flags) = fields
        wrapper_shape = (
            image_size == 0 and data_size > 0 and
            width == height == width_ln2 == height_ln2 == 0 and
            width_minus_1 == height_minus_1 == palette_id == palette_id_2 == 0 and
            bool(flags & 0x100) and
            PSEUDO_IMAGE_HEADER.size + data_size <= len(raw)
        )
        if wrapper_shape:
            payload = raw[PSEUDO_IMAGE_HEADER.size:PSEUDO_IMAGE_HEADER.size + data_size]
            value = zlib.decompress(payload) if decompressed_size else payload
            if decompressed_size and len(value) != decompressed_size:
                raise ValueError("LOD image-wrapper decompressed-size mismatch")
            decoded.append(("decoded-image-payload", value))

    # MM8 EnglishD/EnglishT archives use the same pseudo-image fields after a
    # 64-byte name, producing a 96-byte wrapper instead of the common 48-byte
    # one. The payload contains resources such as WAV and text files.
    if len(raw) >= WIDE_PSEUDO_IMAGE_HEADER.size:
        fields = WIDE_PSEUDO_IMAGE_HEADER.unpack_from(raw)
        (_name, image_size, data_size, width, height, width_ln2, height_ln2,
         width_minus_1, height_minus_1, palette_id, another_palette_id,
         decompressed_size, flags) = fields
        wrapper_shape = (
            image_size == 0 and data_size > 0 and
            width == height == width_ln2 == height_ln2 == 0 and
            palette_id == another_palette_id == 0 and
            bool(flags & 0x100) and
            WIDE_PSEUDO_IMAGE_HEADER.size + data_size <= len(raw)
        )
        if wrapper_shape:
            payload = raw[WIDE_PSEUDO_IMAGE_HEADER.size:
                          WIDE_PSEUDO_IMAGE_HEADER.size + data_size]
            value = zlib.decompress(payload) if decompressed_size else payload
            if decompressed_size and len(value) != decompressed_size:
                raise ValueError("wide LOD wrapper decompressed-size mismatch")
            decoded.append(("decoded-wide-image-payload", value))
    return decoded


def encode_bmp8(width: int, height: int, pixels: bytes, palette_rgb: bytes,
                horizontal_flip: bool = False) -> bytes:
    """Encode the 8-bit BMP convention used by the OpenYAMM asset tree."""
    if len(pixels) < width * height:
        raise ValueError("not enough indexed pixels for BMP")
    if len(palette_rgb) != 256 * 3:
        raise ValueError("8-bit BMP needs a 256-entry RGB palette")
    row_size = (width + 3) & ~3
    image_size = row_size * height
    pixel_offset = 14 + 40 + 256 * 4
    result = bytearray(struct.pack("<2sIHHI", b"BM", pixel_offset + image_size, 0, 0, pixel_offset))
    result.extend(struct.pack("<IiiHHIIiiII", 40, width, height, 1, 8, 0,
                              image_size, 0, 0, 256, 0))
    for index in range(256):
        red, green, blue = palette_rgb[index * 3:index * 3 + 3]
        result.extend((blue, green, red, 0))
    padding = b"\0" * (row_size - width)
    for y in range(height - 1, -1, -1):
        row = pixels[y * width:(y + 1) * width]
        result.extend(row[::-1] if horizontal_flip else row)
        result.extend(padding)
    return bytes(result)


def decode_lod_image_as_bmp(raw: bytes, horizontal_flip: bool = False) -> bytes | None:
    """Decode a LOD paletted image into OpenYAMM's exact BMP convention."""
    header_size = 0
    image_size = data_size = width = height = decompressed_size = 0
    # Normal image LODs use a 16-byte name; MM8 EnglishD/EnglishT uses the
    # same record with a 64-byte name.  The latter contains many UI images
    # and was previously mistaken for an opaque project transform.
    for header in (PSEUDO_IMAGE_HEADER, WIDE_PSEUDO_IMAGE_HEADER):
        if len(raw) < header.size:
            continue
        fields = header.unpack_from(raw)
        (_name, candidate_image_size, candidate_data_size,
         candidate_width, candidate_height, _width_ln2, _height_ln2,
         _width_minus_1, _height_minus_1, _palette_id,
         _another_palette_id, candidate_decompressed_size, _flags) = fields
        if (
            candidate_image_size > 0 and candidate_data_size > 0
            and candidate_width > 0 and candidate_height > 0
            and candidate_image_size == candidate_width * candidate_height
            and header.size + candidate_data_size + 768 == len(raw)
        ):
            header_size = header.size
            image_size = candidate_image_size
            data_size = candidate_data_size
            width = candidate_width
            height = candidate_height
            decompressed_size = candidate_decompressed_size
            break
    if not header_size:
        return None
    packed = raw[header_size:header_size + data_size]
    pixels = zlib.decompress(packed) if decompressed_size else packed
    if decompressed_size and len(pixels) != decompressed_size:
        raise ValueError("LOD image decompressed-size mismatch")
    if len(pixels) < image_size:
        raise ValueError("LOD image pixel data is too short")
    palette = raw[header_size + data_size:header_size + data_size + 768]
    return encode_bmp8(width, height, pixels[:image_size], palette, horizontal_flip)


def decode_lod_palette(raw: bytes) -> bytes | None:
    if len(raw) != PSEUDO_IMAGE_HEADER.size + 768:
        return None
    fields = PSEUDO_IMAGE_HEADER.unpack_from(raw)
    if any(fields[index] != 0 for index in range(1, len(fields))):
        return None
    return raw[PSEUDO_IMAGE_HEADER.size:]


def decode_lod_sprite_as_bmp(raw: bytes, palettes: dict[int, bytes],
                             horizontal_flip: bool = False) -> bytes | None:
    """Decode a LOD sprite, honoring the format's inclusive scanline end."""
    if len(raw) < LOD_SPRITE_HEADER.size:
        return None
    (_name, data_size, width, height, palette_id, unknown, empty_bottom,
     _flags, decompressed_size) = LOD_SPRITE_HEADER.unpack_from(raw)
    header_size = LOD_SPRITE_HEADER.size + height * LOD_SPRITE_LINE.size
    if not (data_size > 0 and width > 0 and height > 0 and palette_id > 0 and
            unknown == 0 and empty_bottom <= height and
            header_size + data_size == len(raw)):
        return None
    palette = palettes.get(palette_id)
    if palette is None:
        return None
    packed = raw[header_size:header_size + data_size]
    pixels_blob = zlib.decompress(packed) if decompressed_size else packed
    if decompressed_size and len(pixels_blob) != decompressed_size:
        raise ValueError("LOD sprite decompressed-size mismatch")
    pixels = bytearray(width * height)
    lines = [LOD_SPRITE_LINE.unpack_from(
        raw, LOD_SPRITE_HEADER.size + y * LOD_SPRITE_LINE.size)
        for y in range(height)]
    for y, (begin, end, offset) in enumerate(lines):
        if y >= height - empty_bottom or begin < 0 or end < 0:
            continue
        next_offset = len(pixels_blob)
        for next_y in range(y + 1, height - empty_bottom):
            if lines[next_y][0] >= 0 and lines[next_y][1] >= 0:
                next_offset = lines[next_y][2]
                break
        # Offset deltas distinguish a genuinely empty line from a one-pixel
        # line whose begin and end coordinates are equal. For populated lines
        # the shipped format uses an inclusive end coordinate.
        count = next_offset - offset
        if count == 0:
            continue
        if (end < begin or begin + count > width or offset < 0 or
                next_offset < offset or next_offset > len(pixels_blob)):
            raise ValueError(f"invalid LOD sprite scanline {y}")
        pixels[y * width + begin:y * width + begin + count] = pixels_blob[offset:offset + count]
    return encode_bmp8(width, height, pixels, palette, horizontal_flip)


def iter_lod_records(path: Path) -> Iterator[tuple[str, str, bytes]]:
    """Yield (qualified name, leaf name, raw bytes) from a LOD."""
    file_size = path.stat().st_size
    with path.open("rb") as stream:
        header_data = stream.read(LOD_HEADER.size)
        if len(header_data) != LOD_HEADER.size:
            raise ValueError("short LOD header")
        signature, version_raw, _description, _size, _unk, directory_count, _tail = LOD_HEADER.unpack(header_data)
        if signature != b"LOD\0":
            raise ValueError(f"bad LOD signature {signature!r}")
        version = c_string(version_raw).upper()
        if not directory_count or directory_count > 64:
            raise ValueError(f"implausible LOD directory count {directory_count}")
        roots = []
        for _ in range(directory_count):
            data = stream.read(LOD_ENTRY_MM6.size)
            if len(data) != LOD_ENTRY_MM6.size:
                raise ValueError("short LOD root directory")
            name, data_offset, data_size, _unknown, item_count, _priority = LOD_ENTRY_MM6.unpack(data)
            roots.append((c_string(name), data_offset, data_size, item_count))
        entry_struct = LOD_ENTRY_MM8 if "VIII" in version else LOD_ENTRY_MM6
        for root_name, root_offset, _root_size, item_count in roots:
            if root_offset + item_count * entry_struct.size > file_size:
                raise ValueError(f"LOD directory outside file: {root_name}")
            stream.seek(root_offset)
            entry_bytes = stream.read(item_count * entry_struct.size)
            for index in range(item_count):
                values = entry_struct.unpack_from(entry_bytes, index * entry_struct.size)
                entry_name = c_string(values[0])
                if entry_struct is LOD_ENTRY_MM8:
                    data_offset, data_size = values[13], values[14]
                else:
                    data_offset, data_size = values[1], values[2]
                absolute = root_offset + data_offset
                if absolute < 0 or data_size < 0 or absolute + data_size > file_size:
                    raise ValueError(f"LOD entry outside file: {entry_name}")
                stream.seek(absolute)
                raw = stream.read(data_size)
                qualified = f"{root_name}/{entry_name}" if root_name else entry_name
                yield qualified, entry_name, raw


def find_game_palettes(root: Path, warnings: list[dict], game: str) -> dict[int, bytes]:
    palettes: dict[int, bytes] = {}
    bitmap_lods = [path for path in walk_files(root) if path.name.lower() == "bitmaps.lod"]
    for path in bitmap_lods:
        try:
            for _qualified, name, raw in iter_lod_records(path):
                lower = name.lower()
                if len(lower) == 6 and lower.startswith("pal") and lower[3:].isdigit():
                    palette = decode_lod_palette(raw)
                    if palette is not None:
                        palettes[int(lower[3:])] = palette
        except Exception as exc:
            warnings.append({"game": game, "path": str(path),
                             "warning": f"could not index palettes: {exc}"})
    return palettes


def iter_lod(path: Path, game: str, relative_path: str,
             palette_sets: dict[str, dict[int, bytes]], warnings: list[dict]) -> Iterator[SourceBlob]:
    for qualified, _entry_name, raw in iter_lod_records(path):
        raw_hash = hashlib.sha256(raw).hexdigest()
        yield SourceBlob(game, "lod", relative_path, qualified, "raw", len(raw), raw_hash)
        for representation, value in decode_lod_record(raw):
            yield SourceBlob(game, "lod", relative_path, qualified, representation,
                             len(value), hashlib.sha256(value).hexdigest())
        try:
            image = decode_lod_image_as_bmp(raw)
        except Exception as exc:
            warnings.append({"game": game, "path": f"{relative_path}:{qualified}",
                             "warning": f"image conversion skipped: {exc}"})
            image = None
        if image is not None:
            yield SourceBlob(game, "lod", relative_path, qualified, "bmp-image",
                             len(image), hashlib.sha256(image).hexdigest())
            flipped = decode_lod_image_as_bmp(raw, horizontal_flip=True)
            if flipped != image:
                yield SourceBlob(game, "lod", relative_path, qualified, "bmp-image-hflip",
                                 len(flipped), hashlib.sha256(flipped).hexdigest())
        for palette_game, palettes in palette_sets.items():
            try:
                sprite = decode_lod_sprite_as_bmp(raw, palettes)
            except Exception as exc:
                if palette_game == game:
                    warnings.append({"game": game, "path": f"{relative_path}:{qualified}",
                                     "warning": f"sprite conversion skipped: {exc}"})
                sprite = None
            if sprite is not None:
                representation = "bmp-sprite" if palette_game == game else f"bmp-sprite-pal-{palette_game}"
                yield SourceBlob(game, "lod", relative_path, qualified, representation,
                                 len(sprite), hashlib.sha256(sprite).hexdigest())
                flipped = decode_lod_sprite_as_bmp(raw, palettes, horizontal_flip=True)
                if flipped != sprite:
                    yield SourceBlob(game, "lod", relative_path, qualified, representation + "-hflip",
                                     len(flipped), hashlib.sha256(flipped).hexdigest())


def iter_vid(path: Path, game: str, relative_path: str) -> Iterator[SourceBlob]:
    file_size = path.stat().st_size
    with path.open("rb") as stream:
        count_data = stream.read(4)
        if len(count_data) != 4:
            raise ValueError("short VID header")
        count = struct.unpack("<I", count_data)[0]
        if count > 100000 or 4 + count * VID_ENTRY.size > file_size:
            raise ValueError(f"implausible VID entry count {count}")
        entries = []
        for _ in range(count):
            name, offset = VID_ENTRY.unpack(stream.read(VID_ENTRY.size))
            entries.append((c_string(name), offset))
        for index, (name, offset) in enumerate(entries):
            end = entries[index + 1][1] if index + 1 < len(entries) else file_size
            if offset > end or end > file_size:
                raise ValueError(f"VID entry outside file: {name}")
            stream.seek(offset)
            digest, size = sha_stream(stream, end - offset)
            yield SourceBlob(game, "vid", relative_path, name, "raw", size, digest)


def iter_snd(path: Path, game: str, relative_path: str,
             warnings: list[dict]) -> Iterator[SourceBlob]:
    file_size = path.stat().st_size
    with path.open("rb") as stream:
        count_data = stream.read(4)
        if len(count_data) != 4:
            raise ValueError("short SND header")
        count = struct.unpack("<I", count_data)[0]
        if count > 100000 or 4 + count * SND_ENTRY.size > file_size:
            raise ValueError(f"implausible SND entry count {count}")
        entries = []
        for _ in range(count):
            name, offset, size, decompressed_size = SND_ENTRY.unpack(stream.read(SND_ENTRY.size))
            entries.append((c_string(name), offset, size, decompressed_size))
        for name, offset, size, decompressed_size in entries:
            if offset + size > file_size:
                raise ValueError(f"SND entry outside file: {name}")
            stream.seek(offset)
            raw = stream.read(size)
            yield SourceBlob(game, "snd", relative_path, name, "raw", size,
                             hashlib.sha256(raw).hexdigest())
            if decompressed_size and decompressed_size != size:
                try:
                    value = zlib.decompress(raw)
                except zlib.error as exc:
                    # Some shipped SND entries are damaged. OpenEnroth also
                    # treats these as best-effort data. Preserve the raw entry
                    # in the inventory and skip only this decoded view.
                    warnings.append({
                        "game": game,
                        "path": f"{relative_path}:{name}",
                        "warning": f"compressed sound could not be decoded: {exc}",
                    })
                    continue
                if len(value) != decompressed_size:
                    warnings.append({
                        "game": game,
                        "path": f"{relative_path}:{name}",
                        "warning": (f"decoded sound size {len(value)} does not match "
                                    f"declared size {decompressed_size}"),
                    })
                    continue
                yield SourceBlob(game, "snd", relative_path, name, "decoded",
                                 len(value), hashlib.sha256(value).hexdigest())


def normalized_name_keys(name: str) -> set[str]:
    base = Path(name.replace("\\", "/")).name.lower()
    stem = Path(base).stem
    keys = {base, stem}
    # OpenYAMM frequently exports original resources to PNG/BMP/WAV/YML/Lua.
    for suffix in (".bmp", ".png", ".pcx", ".wav", ".mp3", ".ogg", ".yml", ".yaml", ".lua", ".evt", ".str"):
        if base.endswith(suffix):
            keys.add(base[:-len(suffix)])
    return {key for key in keys if key}


def walk_files(root: Path) -> Iterator[Path]:
    for directory, dirnames, filenames in os.walk(root):
        dirnames.sort(key=str.lower)
        filenames.sort(key=str.lower)
        for filename in filenames:
            yield Path(directory) / filename


def scan_game(game: str, root: Path, errors: list[dict],
              warnings: list[dict],
              palette_sets: dict[str, dict[int, bytes]]) -> list[SourceBlob]:
    blobs: list[SourceBlob] = []
    for path in walk_files(root):
        relative = path.relative_to(root).as_posix()
        suffix = path.suffix.lower()
        try:
            if suffix == ".lod":
                blobs.extend(iter_lod(path, game, relative, palette_sets, warnings))
            elif suffix == ".vid":
                blobs.extend(iter_vid(path, game, relative))
            elif suffix == ".snd":
                blobs.extend(iter_snd(path, game, relative, warnings))
            else:
                digest, size = sha_file(path)
                blobs.append(SourceBlob(game, "loose", relative, relative, "raw", size, digest))
        except Exception as exc:
            errors.append({"game": game, "path": relative, "error": str(exc)})
    return blobs


def classify_special(asset_relative: str) -> str | None:
    lower = asset_relative.lower()
    name = Path(lower).name
    if "_mmmerge." in name or "_mmerge." in name:
        return "openyamm-overlay"
    if name.endswith("_1.lua") or name.endswith("_1.yml") or name.endswith("_1.yaml"):
        return "openyamm-overlay"
    if "/_legacy/" in f"/{lower}":
        return None
    return None


def choose_recipe_source(asset_path: str, matches: list[SourceBlob],
                         converted: bool) -> SourceBlob | None:
    if not matches:
        return None
    preferred_game = None
    lower = asset_path.lower()
    for game in ("mm6", "mm7", "mm8"):
        if lower.startswith(f"worlds/{game}/"):
            preferred_game = game
            break
    game_order = ([preferred_game] if preferred_game else []) + ["mm8", "mm7", "mm6"]
    game_rank = {game: index for index, game in enumerate(dict.fromkeys(game_order))}
    if converted:
        representation_order = {
            "bmp-image": 0, "bmp-sprite": 1,
            "bmp-image-hflip": 2, "bmp-sprite-hflip": 3,
        }
    else:
        representation_order = {"decoded": 0, "decoded-image-payload": 1,
                                "decoded-wide-image-payload": 2, "raw": 3}
    return min(matches, key=lambda item: (
        game_rank.get(item.game, 99),
        representation_order.get(item.representation, 99),
        0 if item.container_kind == "loose" else 1,
        item.container_path.lower(), item.entry_name.lower(),
    ))


def relative_for_report(path: Path, root: Path) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return str(path)


def write_csv(path: Path, fieldnames: list[str], rows: Iterable[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def scan(args: argparse.Namespace) -> int:
    started = time.time()
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    game_roots = {"mm6": Path(args.mm6).resolve(), "mm7": Path(args.mm7).resolve(), "mm8": Path(args.mm8).resolve()}
    assets_root = Path(args.assets_dev).resolve()
    rod_root = Path(args.rodril).resolve() if args.rodril else None

    missing = [str(path) for path in [*game_roots.values(), assets_root] if not path.is_dir()]
    if missing:
        raise SystemExit("Missing input directories: " + ", ".join(missing))

    errors: list[dict] = []
    warnings: list[dict] = []
    blobs: list[SourceBlob] = []
    palette_sets = {
        game: find_game_palettes(root, warnings, game)
        for game, root in game_roots.items()
    }
    for game, root in game_roots.items():
        print(f"Scanning {game.upper()} at {root}", flush=True)
        blobs.extend(scan_game(game, root, errors, warnings, palette_sets))

    source_by_hash: dict[str, list[SourceBlob]] = defaultdict(list)
    source_by_name: dict[str, list[SourceBlob]] = defaultdict(list)
    for blob in blobs:
        source_by_hash[blob.sha256].append(blob)
        for key in normalized_name_keys(blob.entry_name):
            source_by_name[key].append(blob)

    rod_by_hash: dict[str, list[str]] = defaultdict(list)
    rod_by_name: dict[str, list[str]] = defaultdict(list)
    rod_count = 0
    if rod_root and rod_root.is_dir():
        print(f"Scanning Rodril reference at {rod_root}", flush=True)
        for path in walk_files(rod_root):
            if ".git" in path.parts:
                continue
            rel = path.relative_to(rod_root).as_posix()
            try:
                digest, _size = sha_file(path)
            except OSError as exc:
                errors.append({"game": "rodril", "path": rel, "error": str(exc)})
                continue
            rod_count += 1
            rod_by_hash[digest].append(rel)
            for key in normalized_name_keys(rel):
                rod_by_name[key].append(rel)

    print(f"Hashing OpenYAMM development assets at {assets_root}", flush=True)
    rows: list[dict] = []
    recipe_rows: list[dict] = []
    counts = Counter()
    bytes_by_class = Counter()
    exact_by_game = Counter()
    asset_count = 0
    asset_bytes = 0
    for asset_path in walk_files(assets_root):
        rel = asset_path.relative_to(assets_root).as_posix()
        try:
            digest, size = sha_file(asset_path)
        except OSError as exc:
            errors.append({"game": "assets_dev", "path": rel, "error": str(exc)})
            continue
        asset_count += 1
        asset_bytes += size
        exact = source_by_hash.get(digest, [])
        rod_exact = rod_by_hash.get(digest, [])
        candidates: dict[tuple[str, str, str, str], SourceBlob] = {}
        for key in normalized_name_keys(rel):
            for candidate in source_by_name.get(key, []):
                candidates[(candidate.game, candidate.container_path, candidate.entry_name, candidate.representation)] = candidate
        rod_candidates = set()
        for key in normalized_name_keys(rel):
            rod_candidates.update(rod_by_name.get(key, []))

        special = classify_special(rel)
        direct_exact = [item for item in exact if not item.representation.startswith("bmp-")]
        converted_exact = [item for item in exact if item.representation.startswith("bmp-")]
        if direct_exact:
            classification = "exact-original"
            confidence = "proven"
            for match in exact:
                exact_by_game[match.game] += 1
        elif converted_exact:
            classification = "exact-deterministic-transform"
            confidence = "proven"
            for match in exact:
                exact_by_game[match.game] += 1
        elif rod_exact:
            classification = "exact-rodril"
            confidence = "proven"
        elif special:
            classification = special
            confidence = "structural"
        elif candidates:
            classification = "name-correlated-transform"
            confidence = "candidate"
        elif rod_candidates:
            classification = "name-correlated-rodril"
            confidence = "candidate"
        else:
            classification = "unclassified"
            confidence = "unknown"
        counts[classification] += 1
        bytes_by_class[classification] += size

        source_matches = sorted({
            f"{item.game}:{item.container_kind}:{item.container_path}:{item.entry_name}:{item.representation}"
            for item in exact
        })
        candidate_matches = sorted({
            f"{item.game}:{item.container_kind}:{item.container_path}:{item.entry_name}:{item.representation}"
            for item in candidates.values()
        })
        rows.append({
            "asset_path": rel,
            "size": size,
            "sha256": digest,
            "classification": classification,
            "confidence": confidence,
            "exact_source_matches": " | ".join(source_matches[:20]),
            "name_candidate_matches": " | ".join(candidate_matches[:20]),
            "exact_rodril_matches": " | ".join(sorted(rod_exact)[:20]),
            "name_rodril_candidates": " | ".join(sorted(rod_candidates)[:20]),
        })
        if classification in {"exact-original", "exact-deterministic-transform"}:
            chosen = choose_recipe_source(rel, exact,
                                          classification == "exact-deterministic-transform")
            if chosen is not None:
                recipe_rows.append({
                    "output_path": rel,
                    "output_size": size,
                    "output_sha256": digest,
                    "source_game": chosen.game,
                    "source_kind": chosen.container_kind,
                    "source_container": chosen.container_path,
                    "source_entry": chosen.entry_name,
                    "transform": chosen.representation,
                    "source_representation_sha256": chosen.sha256,
                })

    write_csv(output / "GOG-to-OpenYAMM-provenance.csv", [
        "asset_path", "size", "sha256", "classification", "confidence",
        "exact_source_matches", "name_candidate_matches", "exact_rodril_matches", "name_rodril_candidates"
    ], rows)
    write_csv(output / "GOG-source-entry-inventory.csv", [
        "game", "container_kind", "container_path", "entry_name", "representation", "size", "sha256"
    ], (asdict(blob) for blob in blobs))
    write_csv(output / "GOG-to-OpenYAMM-build-recipe.csv", [
        "output_path", "output_size", "output_sha256", "source_game", "source_kind",
        "source_container", "source_entry", "transform", "source_representation_sha256"
    ], recipe_rows)
    if errors:
        write_csv(output / "GOG-source-scan-errors.csv", ["game", "path", "error"], errors)
    elif (output / "GOG-source-scan-errors.csv").exists():
        (output / "GOG-source-scan-errors.csv").unlink()
    if warnings:
        write_csv(output / "GOG-source-scan-warnings.csv", ["game", "path", "warning"], warnings)
    elif (output / "GOG-source-scan-warnings.csv").exists():
        (output / "GOG-source-scan-warnings.csv").unlink()

    source_counts = Counter((blob.game, blob.container_kind, blob.representation) for blob in blobs)
    summary = {
        "generated_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "duration_seconds": round(time.time() - started, 2),
        "inputs": {
            **{game: str(root) for game, root in game_roots.items()},
            "assets_dev": str(assets_root),
            "rodril": str(rod_root) if rod_root else None,
        },
        "source_representations": len(blobs),
        "source_counts": [
            {"game": key[0], "kind": key[1], "representation": key[2], "count": value}
            for key, value in sorted(source_counts.items())
        ],
        "assets": {"count": asset_count, "bytes": asset_bytes},
        "recipe": {"count": len(recipe_rows),
                   "bytes": sum(int(row["output_size"]) for row in recipe_rows)},
        "classifications": {
            key: {"count": counts[key], "bytes": bytes_by_class[key]}
            for key in sorted(counts)
        },
        "exact_match_occurrences_by_game": dict(sorted(exact_by_game.items())),
        "rodril_files": rod_count,
        "errors": errors,
        "warnings": warnings,
    }
    (output / "GOG-to-OpenYAMM-provenance-summary.json").write_text(
        json.dumps(summary, indent=2), encoding="utf-8"
    )

    exact_count = counts["exact-original"] + counts["exact-deterministic-transform"]
    exact_bytes = bytes_by_class["exact-original"] + bytes_by_class["exact-deterministic-transform"]
    converted_count = counts["exact-deterministic-transform"]
    correlated_count = counts["name-correlated-transform"]
    overlays = counts["openyamm-overlay"]
    report = f"""# OpenYAMM GOG asset provenance probe

Generated {summary['generated_utc']} from clean, installed GOG copies. The probe is read-only and does not copy game assets into its output.

## Measured result

- OpenYAMM development assets inspected: **{asset_count:,} files** ({asset_bytes / (1024**3):.2f} GiB).
- Original-source representations inspected: **{len(blobs):,}** (loose files plus raw and decoded LOD/VID/SND entries).
- Byte-identical assets proven: **{exact_count:,} files** ({exact_bytes / (1024**2):.2f} MiB, {100 * exact_count / max(asset_count, 1):.2f}% by file count).
- Of those, deterministic image/sprite conversions proven byte-for-byte: **{converted_count:,} files**.
- Name-correlated transform candidates: **{correlated_count:,} files**. These are leads, not proof; format conversion or generation must be reconstructed.
- Explicit OpenYAMM/MMMerge overlay files: **{overlays:,}**. These belong in a redistributable project-data package, subject to upstream licensing.
- Exact Rodril repository matches: **{counts['exact-rodril']:,}**; name-correlated Rodril candidates: **{counts['name-correlated-rodril']:,}**.
- Scan errors: **{len(errors)}**.
- Non-fatal source warnings: **{len(warnings)}**.

## Interpretation

The clean GOG installations provide the original LOD/VID/SND inputs, but OpenYAMM's runtime tree is not a byte-for-byte extraction. Exact matches establish direct provenance. Name-only matches identify likely conversions such as image/audio export, event/script generation, scene descriptors, and table conversion; each conversion still needs a deterministic implementation and validation against the known-good `assets_dev` tree.

This result supports a two-part installer:

1. A redistributable OpenYAMM-specific package containing only project-authored generators, overlays, tables and scripts that cannot be derived from the original games.
2. A local builder that reads the user's MM6/MM7/MM8 installations, exports the original resources, applies deterministic transforms and overlays, validates hashes or semantic manifests, and produces the runtime ZIPs.

## Files

- `GOG-to-OpenYAMM-provenance.csv`: one row per OpenYAMM development asset.
- `GOG-source-entry-inventory.csv`: every original loose file and archive representation inspected.
- `GOG-to-OpenYAMM-build-recipe.csv`: distributable reconstruction instructions for every proven file.
- `GOG-to-OpenYAMM-provenance-summary.json`: machine-readable totals and inputs.
- `GOG-source-scan-errors.csv`: parser/read failures, when present.
- `GOG-source-scan-warnings.csv`: damaged source records skipped only for decoded matching, when present.

## Limits

An exact match proves the source bytes are available from the user's installation. A name correlation does not prove the transform. Unclassified files may be generated, renamed, combined, hand-authored, or derived from a source whose format is not decoded by this first probe. Runtime packaging is intentionally outside this milestone.
"""
    (output / "GOG-to-OpenYAMM-provenance-report.md").write_text(report, encoding="utf-8")
    console_summary = dict(summary)
    console_summary["errors"] = len(errors)
    console_summary["warnings"] = len(warnings)
    print(json.dumps(console_summary, indent=2), flush=True)
    return 0 if not errors else 2


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mm6", required=True, help="Clean MM6 installation directory")
    parser.add_argument("--mm7", required=True, help="Clean MM7 installation directory")
    parser.add_argument("--mm8", required=True, help="Clean MM8 installation directory")
    parser.add_argument("--assets-dev", required=True, help="Known-good OpenYAMM assets_dev directory")
    parser.add_argument("--rodril", help="Optional Rodril MMMerge source tree")
    parser.add_argument("--output", required=True, help="Directory for CSV/JSON/Markdown reports")
    return parser


def main() -> int:
    return scan(build_parser().parse_args())


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
