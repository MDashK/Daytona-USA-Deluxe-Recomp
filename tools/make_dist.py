#!/usr/bin/env python3
"""make_dist.py - assemble what gets published:

  _out/github/   the source tree for the GitHub repository (an explicit list:
                 no game files, no build outputs, no logs, no old experiments)
  _out/release/DaytonaUSADX/   the files for a release
      PSVita/dayusadx.vpk, Windows/DaytonaUSADX.exe, Converter/DayConv.exe,
      README.txt
  _out/DaytonaUSADX_release.zip

Build first: python host/build.py, vita/build.sh + package.sh (WSL), and
dotnet publish in tools/converter/DayConv (see docs/TECHNICAL.md).
"""
import fnmatch, os, shutil, sys, zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
OUT = os.path.join(ROOT, '_out')

# (folder, patterns, recursive) - relative to the repository root
SOURCE = [
    ('.', ['README.md'], False),
    ('docs', ['TECHNICAL.md', 'reverse-engineering.md'], False),
    ('recomp', ['*.py', 'overrides.txt', 'fixups*.txt', 'fake_addrs*.txt', 'uninit_calls.txt'], False),
    ('recomp/include', ['*.h'], True),
    ('recomp/export', ['*'], True),
    ('recomp/export_ddse', ['*'], True),
    ('recomp/gen', ['*.c', '*.h'], False),
    ('recomp/gen_ddse', ['*.c', '*.h'], False),
    ('recomp/lifted', ['*.c'], False),
    ('recomp/overrides', ['*.c'], False),
    ('runtime', ['*.c', '*.h'], False),
    ('host', ['build.py', 'build_host.bat', 'plat_win.c', 'daytona.rc'], False),
    ('vita', ['CMakeLists.txt', 'build.sh', 'package.sh', 'plat_vita.c'], False),
    ('ghidra', ['RecompExport.java', 'ListOverrides.java', 'DecompOne.java', 'run_recomp_export.bat',
                'run_recomp_export_ddse.bat', 'list_overrides.bat', 'decomp_one.bat'], False),
    ('tools', ['*.py'], False),
    ('tools/third_party', ['*'], True),
    ('tools/oracle', ['*.py', '*.c', 'probes_*.txt'], False),
    ('tools/converter', ['README.md', '*.py'], False),
    ('tools/converter/DayConv', ['*.cs', '*.csproj', 'app.ico'], False),
]
NEVER = ['*.exe', '*.dll', '*.obj', '*.pdb', '*.bin', '*.vpk', '*.log', '*_log.txt', 'registry.dat', '*.pyc']

GITIGNORE = """# The game: copyrighted, made by the user from their own CD, never published
game/
data/
*.mp3

# Build outputs
build_host/
vita/build/
vita/vpk/
*.vpk
*.obj
*.pdb
*.ilk
*.exe
*.dll
tools/converter/DayConv/bin/
tools/converter/DayConv/obj/
tools/converter/publish/
# made from the game files by tools/converter/make_patch.py
tools/converter/DayConv/patch.bin

# Logs
*.log
daytona_log.txt
__pycache__/
"""

RELEASE_README = """Daytona USA Deluxe for PS Vita and Windows
==========================================

You need your own original Daytona USA Deluxe CD (any region).
No game files are included.

1. MAKE THE GAME FILES (once, on Windows)
   - Install the .NET 8 Desktop Runtime if you don't have it:
     https://dotnet.microsoft.com/download/dotnet/8.0
   - Run Converter\\DayConv.exe
   - Choose your CD image (.cue of a BIN/CUE, recommended) or
     GAME\\DAYTONA USA Deluxe.exe on the CD, an output folder, Convert.
   - The output folder then holds data\\dayusadx\\ (game files + music).
     An .iso has no CD music: use BIN/CUE or the disc for music.

2. PS VITA
   - Install PSVita\\dayusadx.vpk with VitaShell.
   - Copy the "data" folder from the output folder to ux0:
     (the game must be in ux0:data/dayusadx/).
   - Needs libshacccg.suprx (install it with ShaRKBR33D if missing).

3. WINDOWS
   - Copy Windows\\DaytonaUSADX.exe into the output folder, next to "data",
     and run it. Alt+Enter = fullscreen.

Options menu: press Select (Vita) / Backspace (PC) on the main menu.
Saves and log: data/dayusadx/registry.dat and daytona_log.txt.

Port by MDashK - AI assisted by Claude.
Daytona USA Deluxe (c) SEGA. Not affiliated with or endorsed by SEGA.
"""


def copy_source():
    dst_root = os.path.join(OUT, 'github')
    if os.path.exists(dst_root): shutil.rmtree(dst_root)
    n = 0
    for folder, pats, rec in SOURCE:
        src = os.path.join(ROOT, folder)
        walk = os.walk(src) if rec else [(src, [], os.listdir(src))]
        for dp, dn, fn in walk:
            dn[:] = [d for d in dn if d != '__pycache__']
            for f in fn:
                p = os.path.join(dp, f)
                if not os.path.isfile(p): continue
                if not any(fnmatch.fnmatch(f, x) for x in pats): continue
                if any(fnmatch.fnmatch(f.lower(), x) for x in NEVER): continue
                rel = os.path.relpath(p, ROOT)
                o = os.path.join(dst_root, rel)
                os.makedirs(os.path.dirname(o), exist_ok=True)
                shutil.copy2(p, o)
                n += 1
    open(os.path.join(dst_root, '.gitignore'), 'w', newline='\n').write(GITIGNORE)
    print('github: %d files -> %s' % (n, dst_root))


def make_release():
    rel = os.path.join(OUT, 'release', 'DaytonaUSADX')
    if os.path.exists(os.path.dirname(rel)): shutil.rmtree(os.path.dirname(rel))
    items = [
        ('vita/dayusadx.vpk', 'PSVita/dayusadx.vpk'),
        ('build_host/daytona.exe', 'Windows/DaytonaUSADX.exe'),
        ('tools/converter/publish/DayConv.exe', 'Converter/DayConv.exe'),
    ]
    for s, d in items:
        sp = os.path.join(ROOT, s)
        if not os.path.exists(sp): sys.exit('missing (build it first): ' + s)
        os.makedirs(os.path.dirname(os.path.join(rel, d)), exist_ok=True)
        shutil.copy2(sp, os.path.join(rel, d))
    open(os.path.join(rel, 'README.txt'), 'w', newline='\r\n').write(RELEASE_README)
    z = os.path.join(OUT, 'DaytonaUSADX_release.zip')
    with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED) as zf:
        for dp, dn, fn in os.walk(rel):
            for f in fn:
                p = os.path.join(dp, f)
                zf.write(p, os.path.relpath(p, os.path.dirname(rel)))
    print('release: %s (+ %s)' % (rel, z))


if __name__ == '__main__':
    copy_source()
    make_release()
