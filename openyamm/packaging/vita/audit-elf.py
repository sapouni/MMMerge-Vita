"""Check a built Android library against the selected Vita ELF loader's format support."""
import argparse
import collections
import json
from pathlib import Path

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

parser = argparse.ArgumentParser()
parser.add_argument('library', type=Path)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
with args.library.open('rb') as stream:
    elf = ELFFile(stream)
    symbols = elf.get_section_by_name('.dynsym')
    imports = sorted({s.name for s in symbols.iter_symbols() if s['st_shndx'] == 'SHN_UNDEF' and s.name})
    exports = {s.name for s in symbols.iter_symbols() if s['st_shndx'] != 'SHN_UNDEF'}
    relocations = collections.Counter()
    for section in elf.iter_sections():
        if isinstance(section, RelocationSection):
            relocations.update(r['r_info_type'] for r in section.iter_relocations())
    dependencies = [t.needed for t in elf.get_section_by_name('.dynamic').iter_tags('DT_NEEDED')]
    checks = {
        'elf32': elf.elfclass == 32,
        'little_endian': elf.little_endian,
        'arm': elf['e_machine'] == 'EM_ARM',
        'shared_library': elf['e_type'] == 'ET_DYN',
        'loader_entry': 'openyamm_vita_main' in exports,
        'no_static_tls': not any(s['p_type'] == 'PT_TLS' for s in elf.iter_segments()),
        'ordinary_relocation_sections': all(elf.get_section_by_name(n) is not None for n in ['.rel.dyn', '.rel.plt']),
        'supported_relocations': set(relocations) <= {0, 2, 21, 22, 23},
        'no_packed_relocations': not any(elf.get_section_by_name(n) is not None for n in ['.android.rel.dyn', '.relr.dyn']),
        'no_gles3_link': 'libGLESv3.so' not in dependencies,
    }
    result = {
        'library': str(args.library), 'checks': checks,
        'format_checks_pass': all(checks.values()),
        'dependencies': dependencies, 'relocations': dict(relocations), 'imports': imports,
        'note': 'Passing validates binary structure only. Every import needs a correct ABI bridge; runtime is untested.'
    }
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
print(json.dumps({k: v for k, v in result.items() if k != 'imports'}, indent=2))
print(f'{len(imports)} imported symbols')
raise SystemExit(0 if result['format_checks_pass'] else 1)
