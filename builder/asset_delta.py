"""Small, independently implemented COPY/LITERAL patches, with strict bounds."""
import struct
import zlib

MAGIC = b'MMAD1'
MAX_OUTPUT = 64 * 1024 * 1024

def create(source: bytes, target: bytes) -> tuple[bytes, int]:
    if max(len(source), len(target)) > MAX_OUTPUT:
        raise ValueError('asset exceeds patch size limit')
    anchors = {}
    for pos in range(0, len(source) - 15, 8):
        key = source[pos:pos + 16]
        positions = anchors.setdefault(key, [])
        if len(positions) < 6:
            positions.append(pos)
        elif positions[-1] != pos:
            positions[-1] = pos
    output = bytearray(MAGIC + struct.pack('<II', len(source), len(target)))
    pos = literal = copied = 0
    while pos < len(target):
        best_pos = best_len = 0
        for start in anchors.get(target[pos:pos + 16], ()):
            limit = min(len(source) - start, len(target) - pos)
            lo, probe = 16, 32
            # Bound failed comparisons to the short shared prefix. Beginning
            # with half the whole asset repeatedly copies huge slices when
            # many uniform 16-byte runs diverge immediately afterwards.
            while probe <= limit and source[start:start + probe] == target[pos:pos + probe]:
                lo = probe
                probe *= 2
            hi = min(limit, probe - 1)
            if probe > limit and source[start:start + limit] == target[pos:pos + limit]:
                lo = limit
            while lo < hi:
                mid = (lo + hi + 1) // 2
                if source[start:start + mid] == target[pos:pos + mid]:
                    lo = mid
                else:
                    hi = mid - 1
            if lo > best_len:
                best_pos, best_len = start, lo
        if best_len >= 24:
            if pos > literal:
                output.extend(b'L' + struct.pack('<I', pos - literal) + target[literal:pos])
            output.extend(b'C' + struct.pack('<II', best_pos, best_len))
            copied += best_len
            pos += best_len
            literal = pos
        else:
            pos += 1
    if pos > literal:
        output.extend(b'L' + struct.pack('<I', pos - literal) + target[literal:pos])
    return zlib.compress(output, 9), copied

def apply(source: bytes, packed: bytes) -> bytes:
    # Bound decompression before interpreting any project-supplied instructions.
    decoder = zlib.decompressobj()
    data = decoder.decompress(packed, MAX_OUTPUT * 2 + 1)
    if not decoder.eof or decoder.unused_data or len(data) > MAX_OUTPUT * 2:
        raise ValueError('invalid or oversized patch stream')
    if len(data) < 13 or data[:5] != MAGIC:
        raise ValueError('invalid patch header')
    source_size, target_size = struct.unpack_from('<II', data, 5)
    if source_size != len(source) or target_size > MAX_OUTPUT:
        raise ValueError('patch source/output size mismatch')
    output = bytearray()
    pos = 13
    while pos < len(data):
        op = data[pos:pos + 1]
        pos += 1
        if op == b'C':
            if pos + 8 > len(data):
                raise ValueError('truncated COPY')
            offset, size = struct.unpack_from('<II', data, pos)
            pos += 8
            if not size or offset + size > len(source):
                raise ValueError('COPY outside source')
            value = source[offset:offset + size]
        elif op == b'L':
            if pos + 4 > len(data):
                raise ValueError('truncated LITERAL')
            size = struct.unpack_from('<I', data, pos)[0]
            pos += 4
            if not size or pos + size > len(data):
                raise ValueError('LITERAL outside patch')
            value = data[pos:pos + size]
            pos += size
        else:
            raise ValueError('unknown patch operation')
        if len(output) + len(value) > target_size:
            raise ValueError('patch exceeds declared output')
        output.extend(value)
    if len(output) != target_size:
        raise ValueError('patch output is short')
    return bytes(output)
