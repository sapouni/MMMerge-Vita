from pathlib import Path
import hashlib, importlib.util, json, re, shutil, subprocess, zipfile

r = Path.cwd()
source = r / 'work/openyamm'
release = r / 'work/release-candidates/01.06'
guest = r / 'work/build-android-0106-clean'
native = r / 'work/build-vita-0106-clean'
llvm = r / 'work/toolchains/android-ndk-r28c/toolchains/llvm/prebuilt/windows-x86_64/bin'
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()

manifest = json.loads((guest / 'build-manifest.json').read_text())
assert manifest['version'] == '01.06'
frozen = json.loads((guest / 'frozen-inputs.json').read_text())
assert all(sha(Path(p)) == h for p, h in frozen.items()), 'Build inputs changed'
assert len(manifest['objects']) == 291
assert all('-DOPENYAMM_VITA_CLEAN=1' in x['command'] and '-DOPENYAMM_VITA_PROFILE' not in x['command'] for x in manifest['objects'])
with zipfile.ZipFile(release / 'pre-change-source.zip') as z:
    changes = []
    for name in z.namelist():
        old, new = z.read(name), (source / name).read_bytes()
        if old != new:
            changes.append(name)

with zipfile.ZipFile(release / 'pre-change-source.zip') as z:
    known=set(z.namelist())
    for folder in ['engine','game','packaging/vita']:
        for p in (source/folder).rglob('*'):
            if p.is_file() and p.suffix in ['.h','.cpp','.c','.sc','.inl'] and p.relative_to(source).as_posix() not in known:
                changes.append(p.relative_to(source).as_posix())


def run(tool, *args):
    return subprocess.check_output([str(llvm / tool), *map(str, args)], text=True, errors='replace')

app = guest / 'game/CMakeFiles/openyamm_game.dir/app/GameApplication.cpp.o'
scene = guest / 'game/CMakeFiles/openyamm_game.dir/scene/IndoorSceneRuntime.cpp.o'
lines = run('llvm-objdump.exe', '-dr', '--demangle', app).splitlines()
constructor = next(i for i, l in enumerate(lines) if 'R_ARM_THM_CALL' in l and 'IndoorSceneRuntime::IndoorSceneRuntime(' in l)
allocation = max(i for i in range(constructor - 150, constructor) if 'R_ARM_THM_CALL' in lines[i] and 'operator new(unsigned int)' in lines[i])
instructions = '\n'.join(lines[allocation - 4:allocation + 1])
allocated = int(re.search(r'mov(?:w|\.w)\s+r0, #0x([0-9a-f]+)', instructions)[1], 16)
dwarf = run('llvm-dwarfdump.exe', '--name=IndoorSceneRuntime', '--show-children', scene)
required = int(re.search(r'DW_AT_name\s*\("IndoorSceneRuntime"\)\s*DW_AT_byte_size\s*\(0x([0-9a-f]+)\)', dwarf)[1], 16)
assert allocated == required and required >= 10128, (allocated, required)
member = re.search(r'DW_AT_name\s*\("m_mm9TeacherScheduleRuntime"\).*?DW_AT_data_member_location\s*\(0x([0-9a-f]+)\)', dwarf, re.S)
offset = int(member[1], 16)
assert offset + 24 <= allocated
nm = run('llvm-nm.exe', '-C', guest / 'libmain.so')
start = int(re.search(r'^([0-9a-f]+) t OpenYAMM::Game::GameApplication::initializeSelectedMapRuntime\(bool\)$', nm, re.M)[1], 16)
final = run('llvm-objdump.exe', '-d', '--demangle', f'--start-address={start}', f'--stop-address={start + 0x3000}', guest / 'libmain.so')
assert re.search(r'mov(?:w|\.w)\s+r0, #0x' + format(required, 'x') + r'\n[^\n]*<operator new\(unsigned int\)>', final)
(release / 'final-allocation-disassembly.txt').write_text(final)
(release / 'scene-layout-dwarf.txt').write_text(dwarf)
layout = dict(allocated_bytes=allocated, required_bytes=required, schedule_offset=offset,
              schedule_extent=offset + 24, final_executable_allocation_verified=True,
              object_instructions=instructions)
(release / 'layout-verification.json').write_text(json.dumps(layout, indent=2))

# The collider batch adds two fields to the outdoor world. Verify its allocation too.
world_object = guest / 'game/CMakeFiles/openyamm_game.dir/outdoor/OutdoorWorldRuntime.cpp.o'
world_dwarf = run('llvm-dwarfdump.exe', '--name=OutdoorWorldRuntime', '--show-children', world_object)
world_required = int(re.search(r'DW_AT_name\s*\("OutdoorWorldRuntime"\)\s*DW_AT_byte_size\s*\(0x([0-9a-f]+)\)', world_dwarf)[1],16)
# The default constructor is inlined at make_unique in the final executable.
# Match the unique constant-size allocation in initializeSelectedMapRuntime,
# rather than assuming an out-of-line constructor relocation exists.
world_matches = list(re.finditer(r'mov(?:w|\.w)\s+r0, #0x' + format(world_required,'x') + r'\n[^\n]*<operator new\(unsigned int\)>', final))
assert len(world_matches) == 1, (world_required, len(world_matches))
world_insn = world_matches[0].group(0)
world_allocated = int(re.search(r'#0x([0-9a-f]+)',world_insn)[1],16)
assert world_allocated == world_required
layout['outdoor_world'] = dict(allocated_bytes=world_allocated,required_bytes=world_required,final_executable_allocation_verified=True,constructor_inlined=True,final_instructions=world_insn)
(release / 'outdoor-world-dwarf.txt').write_text(world_dwarf)
(release / 'layout-verification.json').write_text(json.dumps(layout, indent=2))

spec = importlib.util.spec_from_file_location('audit', r / 'work/elf32-audit-0041.py')
audit_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit_module)
stripped = release / 'libmain.so'
shutil.copyfile(guest / 'libmain.so', stripped)
subprocess.run([str(llvm / 'llvm-strip.exe'), '--strip-unneeded', str(stripped)], check=True)
provided = set(re.findall(r'\{"([^"\n]+)",', (source / 'packaging/vita/bridge-symbols-generated.c').read_text() + (r / 'work/deps/vitagl-safe-clean-0054/source/lookup.c').read_text()))
audit = audit_module.Elf32(stripped).audit()
assert audit['format_checks_pass']
assert 'oy_path_cooperate' in audit['imports']
assert not set(audit['imports']) - provided, sorted(set(audit['imports']) - provided)
elf = audit_module.Elf32(native / 'openyamm-vita')
symbols = {n for n, index in elf.symbols('.symtab') if index}
assert not [n for n in symbols if n.startswith(('oy_trace_', 'oy_sampler_', 'samplerController', 'oy_profile_'))]
assert not any(n.startswith(('__emutls_v.', 'oy_stall_operation_', 'oy_stall_heap_snapshot')) for n in symbols)
native_disassembly=subprocess.check_output([str(r/'work/toolchains/vitasdk-softfp/bin/arm-vita-eabi-objdump.exe'),'-d','--disassemble=_init_vita_malloc',str(native/'openyamm-vita')],text=True)
assert '<__wrap_sceKernelCreateLwMutex>' in native_disassembly
(release/'heap-startup-disassembly.txt').write_text(native_disassembly)
assert '.debug_info' in elf.sections
assert '.debug_info' in audit_module.Elf32(guest / 'libmain.so').sections





(release / "elf-audit.json").write_text(json.dumps(audit, indent=2))
print("Final ARM layouts, imports, debug symbols, clean-build and heap-lock checks passed")
