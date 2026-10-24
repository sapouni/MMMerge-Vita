"""Assemble an isolated bundled Python runtime and native Windows launchers."""
from pathlib import Path
import json
import shutil
import subprocess
import sys
import zipfile
from builder_backend import sha_file

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DIST = HERE / 'portable/MMMerge Vita Asset Builder'
PYTHON = Path(sys.executable).parent
FFMPEG = ROOT / 'work/toolchains/ffmpeg-release-essentials-extracted/ffmpeg-9.0.2-essentials_build'

def main():
    if DIST.exists():
        raise FileExistsError(DIST)
    internal = DIST / '_internal'
    runtime = internal / 'python'
    runtime.mkdir(parents=True)
    # The isolated ._pth file prevents global Python/site/venv/PATH dependencies.
    for name in ('python.exe', 'python3.dll', 'python312.dll', 'vcruntime140.dll', 'vcruntime140_1.dll'):
        shutil.copyfile(PYTHON / name, runtime / name)
    shutil.copytree(PYTHON / 'DLLs', runtime / 'DLLs', ignore=shutil.ignore_patterns('__pycache__', '*.pyc', '*.pdb'))
    with zipfile.ZipFile(runtime / 'python312.zip', 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for path in sorted((PYTHON / 'Lib').rglob('*.py')):
            relative = path.relative_to(PYTHON / 'Lib')
            if any(p.lower() in ('site-packages', 'test', 'tests', '__pycache__', 'tkinter', 'idlelib', 'ensurepip', 'venv', 'turtledemo') for p in relative.parts):
                continue
            z.write(path, relative.as_posix())
    (runtime / 'python312._pth').write_text('python312.zip\n.\nDLLs\n..\n', encoding='utf-8')
    for name in ('builder_backend.py', 'asset_delta.py', 'source_assets.py', 'openyamm_asset_probe.py'):
        shutil.copyfile(HERE / name, internal / name)
    shutil.copytree(HERE / 'data', internal / 'data')
    (internal / 'ffmpeg').mkdir()
    shutil.copyfile(FFMPEG / 'bin/ffmpeg.exe', internal / 'ffmpeg/ffmpeg.exe')
    compiler = Path('C:/Windows/Microsoft.NET/Framework64/v4.0.30319/csc.exe')
    for source, name, target in (('CoreLauncher.cs', 'MMMergeBuilderCore.exe', 'exe'), ('BuilderGui.cs', 'MMMerge Vita Asset Builder.exe', 'winexe')):
        result = subprocess.run([str(compiler), '/nologo', '/codepage:65001', '/target:' + target, '/optimize+',
            '/reference:System.Windows.Forms.dll', '/reference:System.Drawing.dll',
            '/out:' + str(DIST / name), str(HERE / source)], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
    shutil.copyfile(HERE / 'README.txt', DIST / 'START-HERE.txt')
    licenses = DIST / 'licenses'
    licenses.mkdir()
    shutil.copyfile(PYTHON / 'LICENSE.txt', licenses / 'Python-LICENSE.txt')
    for name in ('LICENSE', 'README.txt'):
        shutil.copyfile(FFMPEG / name, licenses / ('FFmpeg-' + name + '.txt'))
    # Preserve the notices supplied with the game data/tool prototype; do not
    # reinterpret unresolved media as proven project-owned content.
    notice = ('Data reconstruction targets the existing MMMerge/OpenYAMM runtime.\n'
              'The bundled supplemental-data inventory is explicit; not all media provenance is traced.\n'
              'Existing original-game, MMMerge and OpenYAMM rights/notices remain applicable.\n'
              'No original installer or original LOD/SND/VID archive is bundled.\n')
    (licenses / 'Supplemental-data-NOTICE.txt').write_text(notice, encoding='utf-8')
    sources = DIST / 'source'
    sources.mkdir()
    for name in ('CoreLauncher.cs', 'BuilderGui.cs', 'prepare_resources.py', 'package_windows.py'):
        shutil.copyfile(HERE / name, sources / name)
    manifest = [{'path': p.relative_to(DIST).as_posix(), 'size': p.stat().st_size, 'sha256': sha_file(p)}
                for p in sorted(DIST.rglob('*')) if p.is_file()]
    (DIST / 'PACKAGE-FILES.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print('Portable distribution assembled: ' + str(DIST), flush=True)

if __name__ == '__main__':
    main()
