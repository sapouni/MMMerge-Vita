"""Isolated clean/aggregate builds; never installs or changes an earlier build."""
from pathlib import Path
import concurrent.futures, hashlib, json, os, re, shutil, subprocess, sys
r=Path.cwd();variant=sys.argv[1];assert variant in ['clean','aggregate','memorycheck']
base=r/'work/build-android';out=r/f'work/build-android-0106-{variant}';out.mkdir(exist_ok=True)
entries=json.loads((r/'work/compdb-0039-final.json').read_text(encoding='utf-8-sig'))
selected=[e for e in entries if e['file'].endswith('.cpp') and e['output'].endswith('.o')
          and ('/openyamm/' in e['file'].replace('\\','/') or e['output'].startswith('CMakeFiles/openyamm_bgfx.dir/'))]
headers=max(p.stat().st_mtime_ns for folder in ['engine','game'] for p in (r/'work/openyamm'/folder).rglob('*.h'))

# Freeze actual input contents, not output-vs-header timestamps. A header can change
# while a long compiler invocation is still producing its object.
input_files = {Path(e['file']) for e in selected}
for folder in ['engine','game','packaging/vita']:
 input_files.update(p for p in (r/'work/openyamm'/folder).rglob('*') if p.is_file() and p.suffix in ['.h','.cpp','.c','.sc','.inl'])
input_files.update(p for p in (r/"work/deps/bgfx/src").rglob("*") if p.is_file() and p.suffix in [".h", ".cpp"])
frozen_inputs = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in input_files}
(out/'frozen-inputs.json').write_text(json.dumps(frozen_inputs,indent=2))
def assert_frozen_inputs():
 changed=[p for p,h in frozen_inputs.items() if hashlib.sha256(Path(p).read_bytes()).hexdigest()!=h]
 if changed:raise RuntimeError('Inputs changed during build; reject ALL output: '+str(changed))

def dependency_time(dest, entry):
 # Use the compiler's actual include dependencies; a private diagnostic header need not rebuild all 291 units.
 dep=dest.with_suffix('.d')
 if not dep.exists():return max(headers,Path(entry['file']).stat().st_mtime_ns)
 body=dep.read_text().replace('\\\n','').split(': ',1)[1]
 newest=Path(entry['file']).stat().st_mtime_ns
 for token in re.findall(r'(?:\\ |[^\s])+',body):
  path=Path(token.replace('\\ ', ' ').replace('\\:',':'))
  if not path.is_absolute():path=base/path
  if not path.exists():return max(headers,dest.stat().st_mtime_ns+1)
  newest=max(newest,path.stat().st_mtime_ns)
 return newest
def compile_one(e):
 dest=out/e['output'];dest.parent.mkdir(parents=True,exist_ok=True)
 cmd=e['command'].replace(' -DOPENYAMM_VITA_PROFILE=1','')
 cmd+=' -DOPENYAMM_VITA_CLEAN=1'
 if variant=='memorycheck':cmd+=' -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE -fsanitize=array-bounds,object-size -fsanitize-trap=all'
 if variant=='aggregate':cmd+=' -DOPENYAMM_VITA_PROFILE=1 -DOPENYAMM_VITA_AGGREGATE=1'
 cmd=re.sub(r' -o \S+',' -o '+dest.as_posix(),cmd,count=1)
 cmd=re.sub(r' -MF \S+',' -MF '+dest.with_suffix('.d').as_posix(),cmd,count=1)
 key=hashlib.sha256(cmd.encode()).hexdigest();stamp=dest.with_suffix('.command-sha')
 if not (dest.exists() and stamp.exists() and stamp.read_text()==key
         and dest.stat().st_mtime_ns>=dependency_time(dest,e)):
  v=subprocess.run(cmd+' -fstack-usage',cwd=base,capture_output=True,text=True,errors='replace')
  dest.with_suffix('.log').write_text(v.stdout+v.stderr)
  if v.returncode:raise RuntimeError(e['file']+'\n'+v.stderr[-2500:])
  stamp.write_text(key)
 return dict(file=e['file'],object=e['output'],source_sha256=hashlib.sha256(Path(e['file']).read_bytes()).hexdigest(),command=cmd)
rows=[]
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
 for row in pool.map(compile_one,selected):
  rows.append(row)
  if len(rows)%30==0:print(variant,'compiled',len(rows),'/',len(selected),flush=True)
assert_frozen_inputs()
llvm=r/'work/toolchains/android-ndk-r28c/toolchains/llvm/prebuilt/windows-x86_64/bin'
libs={'engine/libopenyamm_engine.a':'engine/','game/libopenyamm_game.a':'game/',
      'libopenyamm_bgfx.a':'CMakeFiles/openyamm_bgfx.dir/'}
for name,prefix in libs.items():
 obj=[(out if e in selected else base)/e['output'] for e in entries if e['output'].startswith(prefix)
      and e['output'].endswith('.o') and ' -c ' in e['command']]
 lib=out/name;lib.parent.mkdir(exist_ok=True,parents=True);rsp=out/(Path(name).stem+'.rsp')
 rsp.write_text('\n'.join('"'+p.as_posix()+'"' for p in obj))
 if lib.exists():lib.unlink()
 subprocess.run([str(llvm/'llvm-ar.exe'),'rcs',str(lib),'@'+str(rsp)],check=True)
link=next(e['command'] for e in entries if e['output']=='android/app/src/main/cpp/libmain.so')
link=link[link.index(str(llvm/'clang++.exe')):link.rfind(' && cd .')]
link=re.sub(r' -o \S+',' -o '+(out/'libmain.so').as_posix(),link,count=1)
link=re.sub(r' -Xlinker --dependency-file=\S+','',link)
for e in selected:
 if e['output'].startswith('android/'):link=link.replace(e['output'],(out/e['output']).as_posix())
for name in libs:link=link.replace(name,(out/name).as_posix())
(out/'link-command.txt').write_text(link)
v=subprocess.run(link,cwd=base,capture_output=True,text=True,errors='replace');(out/'link.log').write_text(v.stdout+v.stderr)
if v.returncode:raise RuntimeError(v.stderr[-3000:])
assert_frozen_inputs()
(out/'build-manifest.json').write_text(json.dumps(dict(version='01.06',variant=variant,objects=rows,
    header_hashes={p.relative_to(r).as_posix():hashlib.sha256(p.read_bytes()).hexdigest()
                   for folder in ['engine','game'] for p in (r/'work/openyamm'/folder).rglob('*.h')},
    overhead_verified=False,installed=False),indent=2))
print(variant,'guest linked',flush=True)
