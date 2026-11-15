#!/usr/bin/env python3
"""Incremental, parallel Windows x86 test build (MSVC) -> build_host/daytona.exe

    python host/build.py            build
    python host/build.py run [s]    build, then run for s seconds (log in build_host/run.log)
"""
import os, sys, subprocess, glob, json, concurrent.futures, time, shutil

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
TRACE = os.environ.get('DAYTONA_TRACE') == '1'
OUT = os.path.join(ROOT, 'build_host_trace' if TRACE else 'build_host')
VCVARS = r'E:\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars32.bat'
CFLAGS = ['/nologo', '/c', '/O1', '/Oy-', '/Zi', '/W2', '/FS', '/GS-',
          '/wd4002', '/wd4003', '/wd4018', '/wd4024', '/wd4028', '/wd4047', '/wd4133', '/wd4244',
          '/wd4267', '/wd4305', '/wd4306', '/wd4311', '/wd4312', '/wd4700', '/wd4715', '/wd4716',
          '/wd4723', '/wd4029', '/wd4087', '/D_CRT_SECURE_NO_WARNINGS',
          '/I' + os.path.join(ROOT, 'recomp', 'include'), '/I' + os.path.join(ROOT, 'recomp', 'gen'),
          '/I' + os.path.join(ROOT, 'runtime')]
if TRACE:
    CFLAGS.append('/DRECOMP_TRACE')
LIBS = ['user32.lib', 'gdi32.lib', 'opengl32.lib', 'winmm.lib', 'dbghelp.lib']


def vc_env():
    cache = os.path.join(OUT, 'vcenv.json')
    if os.path.exists(cache):
        return json.load(open(cache))
    out = subprocess.run('cmd /c ""%s" >nul && set"' % VCVARS, capture_output=True, text=True, shell=True).stdout
    env = dict(l.split('=', 1) for l in out.splitlines() if '=' in l)
    json.dump(env, open(cache, 'w'))
    return env


def newest(paths):
    return max(os.path.getmtime(p) for p in paths)


def main():
    os.makedirs(OUT, exist_ok=True)
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'gen_stubs.py')], check=True)
    env = vc_env()
    path = env.get('Path') or env.get('PATH')
    CL = shutil.which('cl', path=path)
    LINK = shutil.which('link', path=path)
    headers = glob.glob(os.path.join(ROOT, 'recomp', 'include', '*.h')) + glob.glob(os.path.join(ROOT, 'recomp', 'include', 'third_party', '*.h')) + glob.glob(os.path.join(ROOT, 'runtime', '*.h'))
    genh = glob.glob(os.path.join(ROOT, 'recomp', 'gen', '*.h'))
    jobs = []
    for src in sorted(glob.glob(os.path.join(ROOT, 'recomp', 'gen', '*.c'))):
        jobs.append((src, os.path.join(OUT, os.path.basename(src)[:-2] + '.obj'), headers + genh))
    for src in sorted(glob.glob(os.path.join(ROOT, 'recomp', 'overrides', '*.c'))):
        jobs.append((src, os.path.join(OUT, 'ovr_' + os.path.basename(src)[:-2] + '.obj'), headers + genh))
    for stale in glob.glob(os.path.join(OUT, 'game_*.obj')):
        if not os.path.exists(os.path.join(ROOT, 'recomp', 'gen', os.path.basename(stale)[:-4] + '.c')):
            os.remove(stale)
    for src in sorted(glob.glob(os.path.join(ROOT, 'recomp', 'lifted', '*.c'))):
        jobs.append((src, os.path.join(OUT, 'lift_' + os.path.basename(src)[:-2] + '.obj'), headers))
    ddseh = glob.glob(os.path.join(ROOT, 'recomp', 'gen_ddse', '*.h'))
    for src in sorted(glob.glob(os.path.join(ROOT, 'recomp', 'gen_ddse', '*.c'))):
        jobs.append((src, os.path.join(OUT, 'ddse_' + os.path.basename(src)[:-2] + '.obj'), headers + ddseh))
    for src in sorted(glob.glob(os.path.join(ROOT, 'runtime', '*.c')) + glob.glob(os.path.join(ROOT, 'host', '*.c'))):
        jobs.append((src, os.path.join(OUT, 'rt_' + os.path.basename(src)[:-2] + '.obj'), headers + genh))
    todo = [j for j in jobs if not os.path.exists(j[1]) or os.path.getmtime(j[1]) < newest([j[0]] + j[2])]

    def compile(j, CL=CL):
        src, obj, _ = j
        r = subprocess.run([CL] + CFLAGS + [src, '/Fo' + obj, '/Fd' + os.path.join(OUT, 'vc.pdb')],
                           capture_output=True, text=True, env=env, errors='replace')
        log = obj[:-4] + '.log'
        open(log, 'w').write(r.stdout + r.stderr)
        return src, r.returncode, r.stdout

    failed = False
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        for src, rc, out in ex.map(compile, todo):
            if rc:
                failed = True
                print('FAIL', os.path.basename(src))
                print('\n'.join(l for l in out.splitlines() if ' error ' in l)[:3000])
    if failed:
        sys.exit(1)
    exe = os.path.join(OUT, 'daytona.exe')
    objs = [j[1] for j in jobs]
    # the icon (host/daytona.rc -> the original game's icon)
    rc_src = os.path.join(ROOT, 'host', 'daytona.rc')
    res = os.path.join(OUT, 'daytona.res')
    ico = os.path.join(ROOT, 'tools', 'converter', 'DayConv', 'app.ico')
    if os.path.exists(rc_src) and os.path.exists(ico):
        if not os.path.exists(res) or os.path.getmtime(res) < newest([rc_src, ico]):
            RC = shutil.which('rc', path=path)
            r = subprocess.run([RC, '/nologo', '/fo', res, rc_src], capture_output=True, text=True, env=env,
                               errors='replace', cwd=os.path.join(ROOT, 'host'))
            if r.returncode:
                print(r.stdout + r.stderr)
                sys.exit(1)
            todo = todo or ['res']
        objs.append(res)
    if todo or not os.path.exists(exe):
        # a windowed program (no console; --console opens one for the log, host/plat_win.c)
        r = subprocess.run([LINK, '/nologo', '/DEBUG', '/SUBSYSTEM:WINDOWS', '/ENTRY:mainCRTStartup', '/MAP:' + exe[:-4] + '.map', '/INCREMENTAL:NO', '/DYNAMICBASE:NO', '/OUT:' + exe] + objs + LIBS,
                           capture_output=True, text=True, env=env, errors='replace')
        if r.returncode:
            print(r.stdout)
            sys.exit(1)
    print('built', exe, '(%d compiled)' % len(todo))
    if len(sys.argv) > 1 and sys.argv[1] == 'run':
        secs = float(sys.argv[2]) if len(sys.argv) > 2 else 15
        e = dict(os.environ, DAYTONA_NOGUI='1')
        with open(os.path.join(OUT, 'run.log'), 'w') as lf:
            p = subprocess.Popen([exe], cwd=OUT, stdout=lf, stderr=subprocess.STDOUT, env=e)
            try:
                p.wait(timeout=secs)
            except subprocess.TimeoutExpired:
                p.kill()
                lf.write('\n[killed after %gs]\n' % secs)


if __name__ == '__main__':
    main()
