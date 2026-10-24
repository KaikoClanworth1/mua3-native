"""Compile build/recomp_full into a prebuilt static library (outside MSBuild, which loses all progress whenever one
compiler dies). Objects are reused while newer than their source; MSVC internal crashes are bisected to a function,
added to noopt.txt and the code regenerated (only changed shards are rewritten)."""
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

# Work dir with nso/ (nsodump output), prof1.txt/prof2.txt (MUA3_RECOMP_PROFILE), noopt.txt/skip.txt
S = os.environ.get('MUA3_RECOMP_WORK', os.path.dirname(os.path.abspath(__file__)))
P = r'D:\Switch Recomp\MUA 3'
GEN = os.path.join(P, 'build', 'recomp_full')
OBJ = os.path.join(P, 'build', 'recomp_full_obj')
VC = os.environ.get('MUA3_VCVARS', r'G:\Programs\Visual Studios\18\Community\VC\Auxiliary\Build\vcvars64.bat')
NOOPT = os.path.join(S, 'noopt.txt')
SKIP = os.path.join(S, 'skip.txt')
open(SKIP, 'a').close()
NSO = os.path.join(S, 'nso')
FLAGS = '/c /nologo /O2 /Ob3 /GS- /EHsc /MT /std:c++20 /permissive- /bigobj /utf-8 /DNDEBUG'
JOBS = int(sys.argv[1]) if len(sys.argv) > 1 else 8


def log(*a):
    print(time.strftime('%H:%M:%S'), *a, flush=True)


def generate():
    cmd = [os.path.join(P, r'build\bin\RelWithDebInfo\recomp.exe'), GEN,
           os.path.join(NSO, 'main.bin'), '8004000', '159d340', os.path.join(NSO, 'sdk.bin'), 'df54000', '51c600',
           os.path.join(NSO, 'subsdk0.bin'), 'cfc5000', '377110', os.path.join(NSO, 'subsdk1.bin'), 'd6e5000', '4ce300',
           *[a for p in sorted(n for n in os.listdir(S) if re.fullmatch(r'prof\d+\.txt', n))
             for a in ('--profile', os.path.join(S, p))], '--all', '1',
           '--noopt', NOOPT, '--skip', SKIP]
    subprocess.run(cmd, stdout=subprocess.DEVNULL, check=True)


def cl(src, obj):
    tag = os.path.basename(obj).replace('.obj', '')
    bat = os.path.join(S, f'cl_{tag}.bat')
    open(bat, 'w').write(f'@call "{VC}" >nul 2>&1\ncl {FLAGS} /I "{P}\\src" /I "{GEN}" /Fo"{obj}" "{src}" >nul 2>&1\n'
                         f'exit /b %ERRORLEVEL%\n')
    rc = subprocess.run(['cmd', '/c', bat]).returncode
    os.remove(bat)
    return rc  # 0 ok, 2 compile error, else compiler crash


def bisect(name):
    src = open(os.path.join(GEN, name)).read()
    head, body = src.split('namespace recomp {\n', 1)
    body = body.rsplit('}', 1)[0]
    funcs = [f for f in re.split(r'\n(?=(?:#pragma optimize\("", off\)\n)?void f_)', body) if f.strip()]
    probe = os.path.join(GEN, 'probe_' + name)  # same dir for the include; not matched by recomp_*.cpp
    pobj = os.path.join(S, 'probe_' + name + '.obj')
    try:
        def crashes(fs):
            open(probe, 'w').write(head + 'namespace recomp {\n' + '\n'.join(fs) + '\n}\n')
            return cl(probe, pobj) not in (0, 2)
        lo = funcs
        while len(lo) > 1:
            a = lo[:len(lo) // 2]
            lo = a if crashes(a) else lo[len(lo) // 2:]
        return re.search(r'void f_([0-9a-f]+)', lo[0]).group(1)
    finally:
        for f in (probe, pobj):
            if os.path.exists(f):
                os.remove(f)


os.makedirs(OBJ, exist_ok=True)
generate()
for attempt in range(10):
    srcs = sorted(n for n in os.listdir(GEN) if re.fullmatch(r'recomp_([0-9a-f]+|table)\.cpp', n))
    todo = [n for n in srcs if not os.path.exists(os.path.join(OBJ, n[:-4] + '.obj')) or
            os.path.getsize(os.path.join(OBJ, n[:-4] + '.obj')) == 0 or
            os.path.getmtime(os.path.join(OBJ, n[:-4] + '.obj')) < os.path.getmtime(os.path.join(GEN, n))]
    log(f'attempt {attempt}: compiling {len(todo)} of {len(srcs)}')
    with ThreadPoolExecutor(JOBS) as ex:
        rcs = list(ex.map(lambda n: cl(os.path.join(GEN, n), os.path.join(OBJ, n[:-4] + '.obj')), todo))
    bad = [n for n, rc in zip(todo, rcs) if rc != 0 or os.path.getsize(os.path.join(OBJ, n[:-4] + '.obj')) == 0]
    errs = [n for n, rc in zip(todo, rcs) if rc == 2]
    if errs:
        log('compile errors (not crashes):', errs)
        sys.exit(1)
    if not bad:
        objs = [os.path.join(OBJ, n[:-4] + '.obj') for n in srcs]
        rsp = os.path.join(OBJ, 'objs.rsp')
        open(rsp, 'w').write('\n'.join(f'"{o}"' for o in objs))
        bat = os.path.join(S, 'lib.bat')
        open(bat, 'w').write(f'@call "{VC}" >nul 2>&1\nlib /nologo /OUT:"{os.path.join(GEN, "nxrecomp_full.lib")}" @"{rsp}"\n'
                             f'exit /b %ERRORLEVEL%\n')
        rc = subprocess.run(['cmd', '/c', bat]).returncode
        log('LIB', 'OK' if rc == 0 else f'failed {rc}')
        sys.exit(rc)
    with ThreadPoolExecutor(JOBS) as ex:
        found = list(ex.map(bisect, bad))
    noopt = open(NOOPT).read().split()
    for n, fn in zip(bad, found):
        # already unoptimised and still crashing: the front end itself fails, leave the function to the JIT
        target = SKIP if fn in noopt else NOOPT
        log(f'{n}: MSVC crashes on f_{fn} -> {os.path.basename(target)}')
        open(target, 'a').write(fn + '\n')
        noopt.append(fn)
    generate()
log('giving up')
sys.exit(1)

