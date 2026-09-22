from pathlib import Path
import hashlib, json, re, struct, zipfile, xml.etree.ElementTree as ET
from PIL import Image
ROOT=Path.cwd();REL=ROOT/'work/release-candidates/01.06';OUT=ROOT/'outputs';SRC=ROOT/'work/openyamm'
sha=lambda b:hashlib.sha256(b).hexdigest()
base=ROOT/'work/release-candidates/01.05/install-r1/MMerge-Vita-01.05-install.vpk'
assert sha(base.read_bytes())=='c088bc95c4213ce35cb03bc725db06d4510031d8cf3d8061938a06fa0856d519'
with zipfile.ZipFile(base) as z:
    assert z.testzip() is None
    app={n:z.read(n) for n in z.namelist() if not n.endswith('/')}
for name in ['eboot.bin','game.self']:
    app[name]=(ROOT/'work/build-vita-0106-clean'/name).read_bytes()
app['libmain.so']=(REL/'libmain.so').read_bytes()
app['assets/vita-ui.zip']=(REL/'vita-ui.zip').read_bytes()
for name in ['settings.ini','vita-performance.ini','vita-guide.txt']:
    app['defaults/'+name]=(REL/'data/openyamm'/name).read_bytes()
for p in (REL/'artwork/app/OYAM00001').rglob('*'):
    if p.is_file():app[p.relative_to(REL/'artwork/app/OYAM00001').as_posix()]=p.read_bytes()
template='sce_sys/livearea/contents/template.xml'
app[template]=app[template].replace(b'content-rev="105"',b'content-rev="106"')
ET.fromstring(app[template])
sfo=bytearray(app['sce_sys/param.sfo']);magic,version,keys,values,count=struct.unpack_from('<5I',sfo);fields={}
assert magic==0x46535000
for i in range(count):
    at=20+16*i;keyoff,kind,length,maximum,valueoff=struct.unpack_from('<HHIII',sfo,at)
    key=bytes(sfo[keys+keyoff:]).split(b'\0',1)[0].decode()
    if key in ['TITLE','STITLE','APP_VER']:
        b=('01.06' if key=='APP_VER' else 'MMerge Vita').encode()+b'\0'
        assert len(b)<=maximum
        sfo[values+valueoff:values+valueoff+maximum]=b+b'\0'*(maximum-len(b))
        struct.pack_into('<I',sfo,at+4,len(b));length=len(b)
    b=bytes(sfo[values+valueoff:values+valueoff+length])
    fields[key]=b.rstrip(b'\0').decode(errors='replace') if kind==0x0204 else b.hex()
assert fields['TITLE_ID']=='OYAM00001' and fields['APP_VER']=='01.06'
app['sce_sys/param.sfo']=bytes(sfo)
for name,size in [('sce_sys/icon0.png',(128,128)),('sce_sys/livearea/contents/bg.png',(840,500)),('sce_sys/livearea/contents/startup.png',(280,158))]:
    import io
    im=Image.open(io.BytesIO(app[name]));assert im.size==size and im.mode=='P'
assert not any(n.startswith(('data/','saves/','assets/worlds/')) or n=='assets/engine.zip' or n.endswith(('.oysav','.pack','.manifest')) for n in app)
for name in ['features-result.json','native-input-result.json','ui-layout-result.json','spellbook-label.json']:
    assert json.loads((ROOT/'work/tests-0106'/name).read_text())['passed']
assert json.loads((REL/'layout-verification.json').read_text())['final_executable_allocation_verified']
guest=ROOT/'work/build-android-0106-clean'
frozen=json.loads((guest/'frozen-inputs.json').read_text())
assert all(sha(Path(p).read_bytes())==h for p,h in frozen.items())
app['build-info.json']=json.dumps(dict(version='01.06',title_id='OYAM00001',mode='normal-clean; opt-in capture',game_data_included=False,
    asset_root='ux0:data/openyamm/assets preferred; app0:assets compatibility',frame_cap_default=0,
    hardware_verified=False,game_sha256=sha(app['libmain.so']),loader_sha256=sha(app['game.self'])),indent=2).encode()
readme='''MMerge Vita 01.06 â€” install package
Normal gameplay build; recording is optional. No Vita test of 01.06 yet.

EXISTING TESTER: ONE-TIME DATA MOVE, THEN INSTALL
1. Fully close the game. Keep your PC backup of the game data and saves.
2. In VitaShell, move the existing folder ux0:app/OYAM00001/assets to
   ux0:data/openyamm/assets. Move the assets folder only, not the app folder.
   If the destination already exists, copy the five archives listed below
   into it instead, and verify they are present before continuing.
   This keeps game data outside the app that the VPK installer updates.
   Keeping extra vita-ui.zip in that folder is harmless: 01.06 always uses
   the port UI from its installed app folder.
3. Copy MMerge-Vita-01.06-install.vpk to the Vita. Exit USB mode and install
   it in VitaShell OVER the existing bubble. Do not uninstall the old game.
4. Copy this ZIP's guide-update/data/openyamm/vita-guide.txt to
   ux0:data/openyamm/vita-guide.txt so the editable guide has the new controls.
   This replaces guide text only. Your saved settings and saves are kept.
5. Return to LiveArea and launch MMerge Vita. If art remains stale, fully
   restart once. The VPK contains the icon, background, transparent logo,
   LiveArea template and title metadata, so this is an actual reinstall of
   those resources rather than another appmeta file-copy workaround.
   Artwork registration still needs confirmation on your Vita.

No other folders need replacing. This VPK does not contain game archives,
saves, replacement user settings, or predecoded sprite/event content packs.
Previously delivered builds remain backed up on the PC.

NEW USERS
Requires a homebrew-enabled Vita, VitaShell, loaded kubridge, and
libshacccg.suprx in ur0:data/ or ur0:data/external/.
Install the VPK, then supply the prepared data in exactly these locations:
  ux0:data/openyamm/assets/engine.zip
  ux0:data/openyamm/assets/worlds/mmmerge.zip
  ux0:data/openyamm/assets/worlds/mm6.zip
  ux0:data/openyamm/assets/worlds/mm7.zip
  ux0:data/openyamm/assets/worlds/mm8.zip
Leave the archives zipped. For OpenYAMM data and project instructions see:
  https://github.com/pjasicek/openyamm
These are OpenYAMM's prepared archives, not unchanged GOG installation files.
This package does not host them or perform original-game conversion.
The tested exported-data revision is recorded in game-data-manifest.json
(upstream 0.11 era). Compatibility with a future data revision is unverified.
Missing settings/guide files are created on first launch; existing ones stay.
First visits can be slower with an empty derived cache. Keep existing caches.

CONTROLS AND WHAT TO TEST
Spellbook: D-pad left by default. Press X once on a learned spell, then
Triangle to assign it to that character. A message confirms the selection.
Triangle again on that same selected spell clears it. Circle still inspects
spells in the book. Close it and use Circle to cast. With no assigned spell,
Circle keeps the existing attack fallback. Assignment is per character.

Vita Settings > Graphics > Gameplay FPS cap: Off, 30, 20 or 60.
This changes immediately; no restart. Off is the fresh-install default.
Menus and the full-screen map are uncapped. A cap limits fast scenes; it
does not make a slow battle faster or change the fixed simulation rate.
The selected limit is included in diagnostic settings snapshots.

Full-screen map: warm image-cache lookup was repaired. World rendering and
actor AI were already paused. Try opening it at the same battle spot, waiting
for the image, panning/zooming, closing and reopening. No measured FPS gain
is claimed. If it stays slow, capture that map-open interval separately.

Use Record diagnostics OFF for FPS comparisons. Enable it only to diagnose;
capture mode can run slower. Existing diagnostic choices are preserved by
installation, so check the setting before your normal test. This is one
build with optional recording, not a separate capture executable.
'''
(REL/'READ-ME-FIRST.txt').write_text(readme,encoding='utf-8')
vpk=REL/'MMerge-Vita-01.06-install.vpk'
with zipfile.ZipFile(vpk,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
    for n,b in sorted(app.items()):z.writestr(n,b)
with zipfile.ZipFile(vpk) as z:
    assert z.testzip() is None and len(z.namelist())==len(set(z.namelist()))
payload={'MMerge-Vita-01.06-install.vpk':vpk.read_bytes(),'READ-ME-FIRST.txt':readme.encode(),
    'guide-update/data/openyamm/vita-guide.txt':app['defaults/vita-guide.txt'],
    'game-data-manifest.json':(ROOT/'work/release-candidates/01.05/install-r1/game-data-manifest.json').read_bytes(),
    '01.06-changes-and-audit.md':(OUT/'OpenYAMM-Vita-01.06-changes-and-audit.md').read_bytes()}
dest=OUT/'MMerge-Vita-01.06-installer.zip'
assert not dest.exists(),'Preserve existing delivered artifacts'
with zipfile.ZipFile(dest,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
    for n,b in payload.items():z.writestr(n,b,compress_type=zipfile.ZIP_STORED if n.endswith('.vpk') else zipfile.ZIP_DEFLATED)
with zipfile.ZipFile(dest) as z:assert z.testzip() is None
manifest=dict(version='01.06',hardware_tested=False,package=str(dest),package_bytes=dest.stat().st_size,package_sha256=sha(dest.read_bytes()),
    vpk_sha256=sha(vpk.read_bytes()),sfo=fields,files={n:dict(bytes=len(b),sha256=sha(b)) for n,b in sorted(app.items())})
(REL/'status.json').write_text(json.dumps(manifest,indent=2))
# Preserve the complete delivered source and native symbols/configuration locally.
with zipfile.ZipFile(REL/'source-at-delivery.zip','w',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
    for folder in ['engine','game','packaging/vita']:
        for p in (SRC/folder).rglob('*'):
            if p.is_file() and p.suffix in ['.h','.c','.cpp','.inl','.sc']:
                z.write(p,p.relative_to(SRC).as_posix())
(REL/'symbols.json').write_text(json.dumps({str(p.relative_to(ROOT)):sha(p.read_bytes()) for p in [guest/'libmain.so',ROOT/'work/build-vita-0106-clean/openyamm-vita',ROOT/'work/build-vita-0106-clean/openyamm-bootstrap',ROOT/'work/build-vita-0106-clean/commands.json',guest/'build-manifest.json']},indent=2))
print(json.dumps({k:v for k,v in manifest.items() if k!='files'},indent=2))
