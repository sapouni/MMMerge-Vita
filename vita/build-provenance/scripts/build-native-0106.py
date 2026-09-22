from pathlib import Path
import re,subprocess,os,json,sys
variant=sys.argv[1];assert variant in ['clean','aggregate']
r=Path.cwd();out=r/('work/build-vita-0106-'+variant);out.mkdir(exist_ok=True)
s=(r/'work/build-vita-0039/build.ninja').read_text()
blocks=re.findall(r'^build ([^\n]+): ([^\n]+)\n((?:  [^\n]*\n)*)',s,re.M)
sdk=r/'work/toolchains/vitasdk-softfp/bin';env=dict(os.environ);env['PATH']=str(sdk)+os.pathsep+str(r/'work/toolchains/msys64/ucrt64/bin')+os.pathsep+env['PATH'];commands=[]
def run(args):
 commands.append(args);v=subprocess.run(args,cwd=out,env=env,capture_output=True,text=True,errors='replace')
 with (out/'build.log').open('a') as f:f.write(v.stdout+v.stderr)
 if v.returncode:raise RuntimeError(v.stderr[-2500:])
for target,desc,body in blocks:
 rule=desc.split()[0]
 if rule.startswith(('C_COMPILER__','ASM_COMPILER__')):
  source=desc.split(' || ')[0].split(maxsplit=1)[1].replace('$:',':')
  values=dict(re.findall(r'^  (\w+) = (.*)$',body,re.M));obj=out/target;obj.parent.mkdir(parents=True,exist_ok=True)
  run([str(sdk/'arm-vita-eabi-gcc.exe'),*[x for x in values.get('DEFINES','').split() if not x.startswith('-DOY_PROFILE_ENABLED=')], '-DOY_PROFILE_ENABLED='+('1' if variant=='aggregate' else '0'), '-DOY_PROFILE_AGGREGATE='+('1' if variant=='aggregate' else '0'),*values.get('INCLUDES','').split(),*values['FLAGS'].split(),'-o',str(obj),'-c',source])
for target,desc,body in blocks:
 if desc.startswith('C_EXECUTABLE_LINKER__'):
  values=dict(re.findall(r'^  (\w+) = (.*)$',body,re.M));objects=desc.split(' | ')[0].split()[1:]
  libraries=values['LINK_LIBRARIES'].replace('deps/vitagl-softfp/libvitaGL.a','deps/vitagl-safe-clean-0054/libvitaGL.a').split()
  run([str(sdk/'arm-vita-eabi-gcc.exe'),*values.get('FLAGS','').split(),*values.get('LINK_FLAGS','').split(),*(['-Wl,--wrap=sceKernelCreateLwMutex','-Wl,--wrap=sceKernelLockLwMutex','-Wl,--wrap=sceKernelUnlockLwMutex','-Wl,--wrap=sceKernelTryLockLwMutex','-Wl,--wrap=__malloc_lock','-Wl,--wrap=__malloc_unlock','-Wl,--wrap=_sbrk_r'] if target=='openyamm-vita' else []),*objects,'-o',target,*values.get('LINK_PATH','').split(),*libraries])
for elf,selfname in [('openyamm-vita','game.self'),('openyamm-bootstrap','eboot.bin')]:
 run([str(sdk/'vita-elf-create.exe'),str(out/elf),str(out/(elf+'.velf'))])
 run([str(sdk/'vita-make-fself.exe'),'-c','-na',str(out/(elf+'.velf')),str(out/selfname)])
(out/'commands.json').write_text(json.dumps(commands,indent=2));print('01.06 native programs built with existing VitaSDK; exact CMake-generated compile/link flags retained')
