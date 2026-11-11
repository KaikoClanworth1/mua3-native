"""Compile build/recomp_full into a prebuilt static library (outside MSBuild, which loses all progress whenever one
compiler dies). Objects are reused while newer than their source; MSVC internal crashes are bisected to a function,
added to noopt.txt and the code regenerated (only changed shards are rewritten)."""
import ctypes
import os
import re
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

P = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # the repository
# Work dir with nso/ (nsodump output), optional prof*.txt / missing*.txt (coverage from game runs), noopt.txt/skip.txt
S = os.environ.get('MUA3_RECOMP_WORK') or os.path.join(P, 'build', 'recomp_work')
# Overridable for a private build next to the shared one (other generator / runtime header under test):
# MUA3_RECOMP_GEN / MUA3_RECOMP_OBJ output dirs, MUA3_RECOMP_EXE generator, MUA3_RECOMP_INC dir holding
# core/cpu/recomp/recomp_rt.h (default src)
GEN = os.environ.get('MUA3_RECOMP_GEN') or os.path.join(P, 'build', 'recomp_full')
OBJ = os.environ.get('MUA3_RECOMP_OBJ') or os.path.join(P, 'build', 'recomp_full_obj')
EXE = os.environ.get('MUA3_RECOMP_EXE') or os.path.join(P, r'build\bin\RelWithDebInfo\recomp.exe')
INC = os.environ.get('MUA3_RECOMP_INC') or os.path.join(P, 'src')


def find_vcvars():
    """vcvars64.bat: MUA3_VCVARS, else the newest Visual Studio with C++ (vswhere)."""
    if os.environ.get('MUA3_VCVARS'):
        return os.environ['MUA3_VCVARS']
    vswhere = os.path.join(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'),
                           'Microsoft Visual Studio', 'Installer', 'vswhere.exe')
    if os.path.exists(vswhere):
        out = subprocess.run([vswhere, '-latest', '-products', '*', '-requires',
                              'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'],
                             capture_output=True, text=True).stdout.strip()
        if out:
            return os.path.join(out, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
    sys.exit('Visual Studio with C++ not found: set MUA3_VCVARS to vcvars64.bat')


VC = find_vcvars()
NOOPT = os.path.join(S, 'noopt.txt')
SKIP = os.path.join(S, 'skip.txt')
# functions whose guest accesses keep reading s.mem (MSVC takes hours on a few with the cached base)
NOGM = os.path.join(S, 'nogm.txt')
# big functions worth /d2OptimizeHugeFunctions (hot in a profile); recomp marks their files (HUGE_MARK)
HUGEOPT = os.path.join(S, 'hugeopt.txt')
os.makedirs(S, exist_ok=True)
open(SKIP, 'a').close()
open(NOOPT, 'a').close()
open(NOGM, 'a').close()
open(HUGEOPT, 'a').close()
NSO = os.path.join(S, 'nso')
if not os.path.exists(os.path.join(NSO, 'main.bin')):
    sys.exit(f'{NSO} has no main.bin: run  build\\bin\\RelWithDebInfo\\nsodump.exe game\\exefs {NSO}  first')
FLAGS = '/c /nologo /O2 /Ob3 /GS- /EHsc /MT /std:c++20 /permissive- /bigobj /utf-8 /DNDEBUG'
# Without it MSVC compiles functions above ~380 KB of C++ (warning C4883) with no optimisation at all (several of them
# hot: 5% of the main thread). On a whole shard it can need over 20 GB, so the generator puts each big function in a
# file of its own, recomp_<shard>_<entry>.cpp, and only those get it. One that takes longer than HUGE_LIMIT with it is
# compiled again without it.
HUGE = '/d2OptimizeHugeFunctions'
HUGE_LIMIT = 60 * 60


HUGE_MARK = '// mua3: optimise huge'


def find_clang():
    """clang-cl: MUA3_CLANG ('0' = MSVC only), else LLVM's, else Visual Studio's (component "C++ Clang Compiler for
    Windows"). It builds the generated code about 10x faster than cl and the result runs faster (it folds SUBS + B.cond
    into cmp + jcc, spills about half as much, and optimises functions of any size)."""
    e = os.environ.get('MUA3_CLANG')
    if e == '0':
        return None
    cands = [e] if e else []
    cands.append(os.path.join(os.environ.get('ProgramFiles', r'C:\Program Files'), 'LLVM', 'bin', 'clang-cl.exe'))
    cands.append(os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(VC))), 'Tools', 'Llvm', 'x64', 'bin',
                              'clang-cl.exe'))
    return next((c for c in cands if c and os.path.exists(c)), None)


CLANG = find_clang()
# The same code as cl: MSVC ABI and headers, no FMA contraction (guest FMUL + FADD round twice), no type-based alias
# analysis (guest memory is one byte array seen through many types), and the instruction sets recomp_rt.h already
# needs (SSE4.1 / SSSE3 / FMA intrinsics).
CLANG_FLAGS = ('/c /nologo /O2 /GS- /EHsc /MT /std:c++20 /bigobj /utf-8 /DNDEBUG -Wno-everything -msse4.2 -mfma -mpopcnt -mpclmul '
               '-ffp-contract=off -fno-strict-aliasing')


def is_big(name):
    return re.fullmatch(r'recomp_[0-9a-f]+_[0-9a-f]+\.(cpp|obj)', os.path.basename(name)) is not None


def is_huge(name):
    """a big-function file recomp marked for /d2OptimizeHugeFunctions (its function is in hugeopt.txt)"""
    if not is_big(name):
        return False
    with open(os.path.join(GEN, os.path.basename(name)[:-4] + '.cpp'), errors='replace') as f:
        return f.readline().startswith(HUGE_MARK)


def free_ram():
    class MS(ctypes.Structure):
        _fields_ = [('len', ctypes.c_ulong), ('load', ctypes.c_ulong), ('total', ctypes.c_ulonglong),
                    ('avail', ctypes.c_ulonglong), ('tpf', ctypes.c_ulonglong), ('apf', ctypes.c_ulonglong),
                    ('tv', ctypes.c_ulonglong), ('av', ctypes.c_ulonglong), ('ave', ctypes.c_ulonglong)]
    m = MS()
    m.len = ctypes.sizeof(MS)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
    return m.avail


def need(n):
    """estimated compiler memory: big-function files only (about 5 GB per MB of C++ with the flag), else 0"""
    if CLANG:
        return 0  # clang-cl: under 1 GB per file
    return max(1 << 30, int(os.path.getsize(os.path.join(GEN, n)) * 5 * 1024)) if is_huge(n) else 0


def compile_all(todo):
    """cl over todo with JOBS workers; big-function files first (largest first) and together within half the RAM
    free at the start (one may always run alone). Returns the exit codes in todo order."""
    budget = free_ram() // 2
    req = [need(n) for n in todo]
    # the optimised big files first, then everything else largest first (no long file left to the end)
    order = sorted(range(len(todo)), key=lambda i: (-req[i], -os.path.getsize(os.path.join(GEN, todo[i]))))
    rcs = [None] * len(todo)
    lock = threading.Condition()
    state = {'reserved': 0, 'next': list(order)}
    nbig = sum(1 for r in req if r)
    if nbig:
        log(f'{nbig} big functions to optimise, within {budget / 2**30:.1f} GB')

    def take():
        with lock:
            while True:
                if not state['next']:
                    return None
                for i in state['next']:
                    if not req[i] or not state['reserved'] or state['reserved'] + req[i] <= budget:
                        state['next'].remove(i)
                        state['reserved'] += req[i]
                        return i
                lock.wait()

    def work():
        while (i := take()) is not None:
            try:
                rcs[i] = cl(os.path.join(GEN, todo[i]), os.path.join(OBJ, todo[i][:-4] + '.obj'))
            finally:
                with lock:
                    state['reserved'] -= req[i]
                    lock.notify_all()

    ts = [threading.Thread(target=work) for _ in range(JOBS)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    return rcs
JOBS = int(sys.argv[1]) if len(sys.argv) > 1 else 8


def log(*a):
    print(time.strftime('%H:%M:%S'), *a, flush=True)


def generate():
    cmd = [EXE, GEN,
           os.path.join(NSO, 'rtld.bin'), '8000000', '19f0',  # process entry: the main thread starts here
           os.path.join(NSO, 'main.bin'), '8004000', '159d340', os.path.join(NSO, 'sdk.bin'), 'df54000', '51c600',
           os.path.join(NSO, 'subsdk0.bin'), 'cfc5000', '377110', os.path.join(NSO, 'subsdk1.bin'), 'd6e5000', '4ce300',
           *[a for p in sorted(n for n in os.listdir(S) if re.fullmatch(r'prof\d+\.txt', n))
             for a in ('--profile', os.path.join(S, p))],
           # recomp_missing.txt files from game runs (pcs the JIT had to run), renamed missing<N>.txt
           *[a for p in sorted(n for n in os.listdir(S) if re.fullmatch(r'missing\d+\.txt', n))
             for a in ('--missing', os.path.join(S, p))], '--all', '1', '--sweep', '1',
           '--noopt', NOOPT, '--skip', SKIP, '--nogm', NOGM, '--hugeopt', HUGEOPT]
    subprocess.run(cmd, stdout=subprocess.DEVNULL, check=True)


def cl(src, obj, huge=None, msvc=False):
    if CLANG and not msvc:
        tag = os.path.basename(obj).replace('.obj', '')
        bat = os.path.join(S, f'cl_{tag}.bat')
        out = os.path.join(S, f'cl_{tag}.log')
        open(bat, 'w').write(f'@call "{VC}" >nul 2>&1\n"{CLANG}" {CLANG_FLAGS} /I "{INC}" /I "{GEN}" /Fo"{obj}" "{src}" '
                             f'>"{out}" 2>&1\nexit /b %ERRORLEVEL%\n')
        rc = subprocess.run(['cmd', '/c', bat]).returncode
        os.remove(bat)
        if rc == 0:
            os.remove(out)
            return 0
        log(f'{tag}: clang-cl failed (code {rc}, {out}), compiling it with cl')
        return cl(src, obj, huge, msvc=True)
    if huge is None:
        huge = is_huge(src)
    tag = os.path.basename(obj).replace('.obj', '')
    bat = os.path.join(S, f'cl_{tag}.bat')
    flags = FLAGS + (' ' + HUGE if huge else '')
    out = os.path.join(S, f'cl_{tag}.log') if huge else 'nul'
    open(bat, 'w').write(f'@call "{VC}" >nul 2>&1\ncl {flags} /I "{INC}" /I "{GEN}" /Fo"{obj}" "{src}" >"{out}" 2>&1\n'
                         f'exit /b %ERRORLEVEL%\n')
    t0 = time.time()
    p = subprocess.Popen(['cmd', '/c', bat])
    try:
        rc = p.wait(timeout=HUGE_LIMIT if huge else None)
    except subprocess.TimeoutExpired:
        subprocess.run(['taskkill', '/T', '/F', '/PID', str(p.pid)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        p.wait()
        log(f'{tag}: over {HUGE_LIMIT // 60} min with {HUGE}, compiling it again without')
        os.remove(bat)
        return cl(src, obj, huge=False)
    os.remove(bat)
    if huge:
        text = open(out, errors='replace').read() if os.path.exists(out) else ''
        if os.path.exists(out):
            os.remove(out)
        if rc != 0 and (rc != 2 or 'C1002' in text or 'C1060' in text):  # out of memory / crash: as before
            log(f'{tag}: {HUGE} failed (code {rc}), compiling it again without')
            return cl(src, obj, huge=False)
        log(f'{tag}: optimised in {(time.time() - t0) / 60:.1f} min')
    return rc  # 0 ok, 2 compile error, else compiler crash


def bisect(name):
    src = open(os.path.join(GEN, name)).read()
    head, body = src.split('namespace recomp {\n', 1)
    body = body.rsplit('}', 1)[0]
    funcs = [f for f in re.split(r'\n(?=(?:#pragma optimize\("", off\)\n)?(?:#define gm s\.mem\n)?void f_)', body) if f.strip()]
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
    srcs = sorted(n for n in os.listdir(GEN) if re.fullmatch(r'recomp_([0-9a-f]+(_[0-9a-f]+)?|table)\.cpp', n))
    todo = [n for n in srcs if not os.path.exists(os.path.join(OBJ, n[:-4] + '.obj')) or
            os.path.getsize(os.path.join(OBJ, n[:-4] + '.obj')) == 0 or
            os.path.getmtime(os.path.join(OBJ, n[:-4] + '.obj')) < os.path.getmtime(os.path.join(GEN, n))]
    log(f'attempt {attempt}: compiling {len(todo)} of {len(srcs)}')
    rcs = compile_all(todo)
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

