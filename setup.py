#!/usr/bin/env python3
"""Madden NFL Arcade PC - build pipeline from YOUR OWN game dump.

    python setup.py --dump "<path to your XBLA package file or extracted game folder>"

Steps: extract the package -> verify the game files -> decrypt Arcadegame.dll -> fetch + patch + build
XenonRecomp -> find jump tables -> recompile the game code to C++ -> build the native host.
Everything derived from the game stays in the build folder on your machine and must not be redistributed.
"""
import argparse, hashlib, os, pathlib, re, shutil, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / 'tools'))
KNOWN_DLL_SHA1 = 'b4e72f27effceee64734d365ed9bb214b0772633'
XENONRECOMP_URL = 'https://github.com/hedge-dev/XenonRecomp.git'
XENONRECOMP_COMMIT = 'ddd128b'
IMAGE_BASE, CODE_LO, CODE_HI = 0x88000000, 0x882A0000, 0x8940B7D4
REQUIRED = ['Arcadegame.dll', 'qkl_boot.ast', 'qkl_fe2ig.ast', 'qkl_interface.ast', 'qkl_misc.ast', 'qkl_ram.ast', 'qkl_stream.ast']

def step(msg): print(f'\n==> {msg}', flush=True)
def run(cmd, **kw):
    print('   $', ' '.join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, **kw)
def sha1(p):
    h = hashlib.sha1()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''): h.update(b)
    return h.hexdigest()

def find_game_files(dump: pathlib.Path, game: pathlib.Path):
    game.mkdir(parents=True, exist_ok=True)
    if dump.is_dir():
        for name in os.listdir(dump):
            src = dump / name
            if src.is_file() and not (game / name).exists():
                try: os.link(src, game / name)
                except OSError: shutil.copy2(src, game / name)
        return
    with open(dump, 'rb') as f: magic = f.read(4)
    if magic in (b'LIVE', b'PIRS', b'CON '):
        from stfs import STFS
        s = STFS(str(dump)); ents = s.entries()
        print(f'   package: {s.title!r} title id {s.title_id:08X}, {len(ents)} entries')
        for e in ents:
            if e['isdir']: continue
            out = game / s.path(ents, e); out.parent.mkdir(parents=True, exist_ok=True)
            if not out.exists():
                print(f'   extracting {s.path(ents, e)} ({e["size"]:,} bytes)'); out.write_bytes(s.read_file(e))
        return
    sys.exit(f'{dump}: not an Xbox 360 package (LIVE/PIRS/CON) or a folder')

def build_xenonrecomp(build: pathlib.Path):
    xr = build / 'XenonRecomp'
    if not (xr / '.git').exists():
        run(['git', 'clone', '--recursive', XENONRECOMP_URL, xr])
        run(['git', '-C', xr, 'checkout', XENONRECOMP_COMMIT])
        run(['git', '-C', xr, 'submodule', 'update', '--init', '--recursive'])
        run(['git', '-C', xr, 'apply', ROOT / 'patches' / 'xenonrecomp_madden.patch'])
    bdir = xr / 'out'
    gen = ['-G', 'Ninja'] if shutil.which('ninja') else []
    cc = ['-DCMAKE_C_COMPILER=clang-cl', '-DCMAKE_CXX_COMPILER=clang-cl'] if os.name == 'nt' else ['-DCMAKE_C_COMPILER=clang', '-DCMAKE_CXX_COMPILER=clang++']
    run(['cmake', '-S', xr, '-B', bdir, *gen, *cc, '-DCMAKE_BUILD_TYPE=Release'])
    run(['cmake', '--build', bdir, '--target', 'XenonRecomp', '--config', 'Release'])
    exe = next((p for p in bdir.rglob('XenonRecomp.exe' if os.name == 'nt' else 'XenonRecomp') if p.is_file()), None)
    if not exe: sys.exit('XenonRecomp build produced no executable')
    return xr, exe

def recompile(build, xr, exe, dll, img):
    ppc = build / 'ppc'; ppc.mkdir(exist_ok=True)
    sw = build / 'switch_tables.toml'
    step('Finding jump tables')
    run([sys.executable, '-I', ROOT / 'tools' / 'findswitch.py', img, hex(IMAGE_BASE), hex(CODE_LO), hex(CODE_HI), sw])
    cfg = build / 'arcade.toml'
    t = (ROOT / 'config' / 'arcade.toml.in').read_text()
    t = t.replace('@IMAGE_SOURCE@', dll.as_posix()).replace('@OUT_DIR@', ppc.as_posix()).replace('@SWITCH_TABLES@', sw.as_posix())
    cfg.write_text(t)
    ctx_h = xr / 'XenonUtils' / 'ppc_context.h'
    for it in range(1, 21):
        step(f'Recompiling PowerPC -> C++ (pass {it})')
        run([sys.executable, '-I', ROOT / 'tools' / 'merge.py', cfg])
        log = build / 'recomp.log'
        with open(log, 'w') as f: subprocess.run([str(exe), str(cfg), str(ctx_h)], stdout=f, stderr=subprocess.STDOUT, check=False)
        errs = len(re.findall('trying to jump outside function', log.read_text(errors='ignore')))
        bad = sum(len(re.findall(r'// ERROR [0-9A-F]{8}', p.read_text(errors='ignore'))) for p in ppc.glob('ppc_recomp.*.cpp'))
        print(f'   jump-table boundary errors: {errs}, branches into other functions: {bad}')
        if errs == 0 and bad == 0: break
        if errs:
            newf = build / 'new_funcs.txt'
            run([sys.executable, '-I', ROOT / 'tools' / 'fixfuncs.py', log, sw, ppc / 'ppc_func_mapping.cpp', img, newf])
            c = cfg.read_text(); i = c.index('functions = [') + len('functions = [\n')
            cfg.write_text(c[:i] + newf.read_text() + c[i:])
            run([sys.executable, '-I', ROOT / 'tools' / 'extend.py', log, ppc / 'ppc_func_mapping.cpp', img, cfg, cfg])
        run([sys.executable, '-I', ROOT / 'tools' / 'fixentries.py', ppc, cfg])
    else:
        sys.exit('could not resolve all function boundaries - please open an issue with build/recomp.log')
    shutil.copy2(ctx_h, ppc / 'ppc_context.h')
    return ppc

def build_host(build, xr, ppc):
    step('Generating import stubs')
    run([sys.executable, '-I', ROOT / 'host' / 'tools' / 'gen_stubs.py', ppc, ROOT / 'host' / 'src', ROOT / 'host' / 'src' / 'stubs_gen.cpp'])
    step('Building the native game (this compiles ~500 large generated files and takes a while)')
    hb = build / 'host'
    gen = ['-G', 'Ninja'] if shutil.which('ninja') else []
    cc = ['-DCMAKE_CXX_COMPILER=clang-cl'] if os.name == 'nt' else ['-DCMAKE_CXX_COMPILER=clang++']
    run(['cmake', '-S', ROOT / 'host', '-B', hb, *gen, *cc, '-DCMAKE_BUILD_TYPE=Release', f'-DPPC_DIR={ppc}', f'-DXENONRECOMP_DIR={xr}'])
    run(['cmake', '--build', hb, '--config', 'Release', '--parallel'])
    exe = next((p for p in hb.rglob('madden_host.exe' if os.name == 'nt' else 'madden_host') if p.is_file()), None)
    return exe

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--dump', required=True, help='your XBLA package file, or a folder with the extracted game files')
    ap.add_argument('--build', default=str(ROOT / 'build'), help='build folder (default: ./build)')
    ap.add_argument('--skip-host', action='store_true', help='stop after recompiling (do not build the native host)')
    a = ap.parse_args()
    build = pathlib.Path(a.build).resolve(); build.mkdir(parents=True, exist_ok=True)
    game = build / 'game'

    step('Reading your game dump'); find_game_files(pathlib.Path(a.dump), game)
    missing = [n for n in REQUIRED if not (game / n).exists()]
    if missing: sys.exit(f'missing game files: {", ".join(missing)}')
    dll = game / 'Arcadegame.dll'
    h = sha1(dll)
    if h != KNOWN_DLL_SHA1:
        print(f'   WARNING: Arcadegame.dll SHA-1 {h} does not match the supported version {KNOWN_DLL_SHA1}; addresses may not line up.')
    else: print('   Arcadegame.dll matches the supported version')

    step('Decrypting Arcadegame.dll')
    from xexload import load
    img_bytes, base, entry = load(str(dll))
    img = game / 'Arcadegame.img'; img.write_bytes(img_bytes)
    print(f'   image {len(img_bytes):,} bytes, base {base:#x}, entry {entry:#x}')

    step('Getting the XenonRecomp static recompiler'); xr, exe = build_xenonrecomp(build)
    ppc = recompile(build, xr, exe, dll, img)
    if a.skip_host: print('\nRecompiled code is in', ppc); return
    host = build_host(build, xr, ppc)
    print('\nDone. Run the game with:\n   ', host, '--game', game)

if __name__ == '__main__':
    main()
