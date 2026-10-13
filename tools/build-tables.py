#!/usr/bin/env python3
"""Build a custom VAWT .elemod with user wavetables, for elekloader.

The factory .elemod ships fixed tables; this bakes a folder of the user's
own single-cycle .wav files (mono 16-bit; each file holds equal frames,
named "<anything> <n>_<framelen>_<frames>_<rate>_16bit.wav") into a
personal dnvawt elemod that combines in elekloader exactly like the
factory one. Pick exactly one dnvawt per build (factory or custom): both
claim the same mod id, sound slot, parameters and page sites.

Usage:
    python3 tools/build-tables.py --wavs DIR --stock STOCK.syx
        [--pick 1-11,25] [--tag X] [--with-builtins] [--out DIR]
        [--version VER] [--patch] [--keep-temp]

    --wavs   flat folder of .wavs (required)
    --stock  stock OS .syx, same file the image will be patched from
             (required; also selects the OS-version ports)
    --pick   table numbers to bake, e.g. 1-11,25 (default: all found)
    --tag    letter shown in the manifest map (default: first alnum char
             of the folder name); display stays WT1..WTN
    --with-builtins prepend the two additive tables as WT1-2 (ordinary entries)
    --out    where the .elemod (+ manifest) goes (default ./custom/)
    --version .elemod version stamp (default: base mod.json version)
    --patch  also build core+machines for the stock file, lint, and write
             a full custom .syx next to the .elemod
    --keep-temp keep the temp mod dir for debugging

Gates, in order: folder/stock/toolchain/numpy checks, table validation
(format, frame math, duplicates, RAM ceiling), generate, sdk.build,
manifest, host DSP tests against the custom header (fail = abort),
optional lint+patch test image.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import wave

ROOT = pathlib.Path(__file__).resolve().parent.parent
EL = ROOT / "tools" / "elekloader"
MOD = ROOT / "vawt"
MAX_BANKS = 24  # resident banks, all replaceable; generate_wt enforces too


def fail(msg, hint=""):
    print(f"ERROR: {msg}")
    if hint:
        print(f"  fix: {hint}")
    raise SystemExit(1)


def parse_pick(spec, available):
    """'1-11,25' -> sorted numbers, all in available (else fail)."""
    if not spec:
        return sorted(available)
    nums = set()
    for part in spec.split(","):
        part = part.strip()
        m = re.fullmatch(r"(\d+)-(\d+)", part)
        if m:
            a, b = int(m.group(1)), int(m.group(2))
            if a > b:
                fail(f"bad --pick range {part!r}")
            nums.update(range(a, b + 1))
        elif re.fullmatch(r"\d+", part):
            nums.add(int(part))
        else:
            fail(f"bad --pick {spec!r}",
                 "example: --pick 1-11,25")
    missing = sorted(n for n in nums if n not in available)
    if missing:
        fail(f"--pick asks for missing tables {missing}",
             f"folder holds {sorted(available)}")
    return sorted(nums)


def load_gen():
    spec = importlib.util.spec_from_file_location(
        "generate_wt", ROOT / "tools" / "generate_wt.py")
    gen = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gen)
    return gen


def collect_flat(wav_dir):
    """{number: path} for the picked tables; warns+skips junk. Name rule
    (see generate_wt.WAV_RE): "<anything> <n>_<framelen>_<frames>_<rate>
    _16bit.wav", mono 16-bit equal single-cycle frames."""
    import numpy  # noqa: F401 (fail fast, clear error below)
    gen = load_gen()
    bynum = {}
    for f in sorted(wav_dir.iterdir(), key=lambda f: f.name):
        if f.suffix != ".wav":
            print(f"  SKIP {f.name} (not a .wav)")
            continue
        m = gen.WAV_RE.fullmatch(f.name)
        if not m:
            print(f"  SKIP {f.name} (not a numbered wavetable)")
            continue
        n = int(m.group(1))
        if n in bynum:
            fail(f"duplicate table number {n} in {wav_dir}")
        bynum[n] = f
    if not bynum:
        fail(f"no wavetables in {wav_dir}")
    return bynum


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--wavs", required=True)
    ap.add_argument("--stock", required=True)
    ap.add_argument("--pick", default="")
    ap.add_argument("--tag", default="")
    ap.add_argument("--with-builtins", action="store_true",
                    help="prepend the two additive tables as WT1-2 (ordinary entries)")
    ap.add_argument("--out", default="custom")
    ap.add_argument("--version", default="")
    ap.add_argument("--patch", action="store_true")
    ap.add_argument("--keep-temp", action="store_true")
    args = ap.parse_args()

    wav_dir = pathlib.Path(args.wavs)
    if not wav_dir.is_dir():
        fail(f"--wavs {wav_dir} is not a directory")
    stock = pathlib.Path(args.stock)
    if not stock.is_file():
        fail(f"--stock {stock} not found",
             "use Elektron's stock OS .syx for your unit")

    try:
        import numpy  # noqa: F401
    except ImportError:
        fail("numpy is missing", "pip install numpy")
    cross = os.environ.get("ELEKLOADER_CROSS", "m68k-elf-")
    if not shutil.which(cross + "gcc"):
        fail(f"{cross}gcc not found",
             "install the m68k toolchain and/or set ELEKLOADER_CROSS")
    if not (EL / "elekloader" / "sdk" / "build.py").is_file():
        fail(f"elekloader sources missing at {EL}",
             "check out tools/elekloader (gitignored reference)")

    tag = args.tag.upper() or next(
        (c for c in wav_dir.name.upper() if c.isalnum()), "C")
    if not re.fullmatch(r"[A-Z0-9]", tag):
        fail(f"bad --tag {args.tag!r}: single A-Z0-9")

    tmp = pathlib.Path(tempfile.mkdtemp(prefix="dnvawt-custom-"))
    try:
        # 1. table numbers present, pick, cap
        bynum = collect_flat(wav_dir)
        if not bynum:
            fail(f"no wavetables in {wav_dir}")
        pick = parse_pick(args.pick, set(bynum))
        extra = 2 if args.with_builtins else 0
        if len(pick) + extra > MAX_BANKS:
            withbi = "+2 additive, " if extra else ""
            fail(f"{len(pick)} tables picked ({withbi}"
                 f"ceiling is {MAX_BANKS} resident)",
                 "narrow --pick; rebuild again for another set")
        print(f"picked {len(pick)}: {pick}")

        # 2. temp mod copy + custom header (24 plain slots)
        work = tmp / "vawt"
        shutil.copytree(MOD, work,
                        ignore=shutil.ignore_patterns("out", ".DS_Store"))
        gen = load_gen()
        banks = []
        if args.with_builtins:
            banks += gen.builtin_banks()
        for n in pick:
            name = f"{tag}{n:02d}"
            banks.append((name, gen.read_user_bank(bynum[n])))
            print(f"  bank {name} <- {bynum[n].name}")
        disp = gen.build_header(work / "wt_tables.h", banks)

        # 3. version stamp
        base = json.loads((MOD / "mod.json").read_text())
        version = args.version or base["version"]
        mj = json.loads((work / "mod.json").read_text())
        mj["version"] = version
        mj["ele3_version"] = version
        (work / "mod.json").write_text(json.dumps(mj, indent=2) + "\n")

        # 4. sdk.build the temp mod
        env = dict(os.environ)
        env.setdefault("ELEKLOADER_CROSS", "m68k-elf-")
        env["PYTHONPATH"] = str(EL) + (
            ":" + env["PYTHONPATH"] if env.get("PYTHONPATH") else "")
        r = subprocess.run(
            [sys.executable, "-m", "elekloader.sdk.build", str(work),
             "--stock", str(stock)],
            cwd=str(ROOT), env=env, capture_output=True, text=True)
        print(r.stdout[-2000:])
        if r.returncode != 0:
            print(r.stderr[-3000:])
            fail("sdk.build failed (see above)")
        arts = sorted((work / "out").glob("dnvawt-*.elemod"),
                      key=lambda p: p.stat().st_mtime)
        arts = [p for p in arts if "-os" not in p.name] or arts
        if not arts:
            fail("sdk.build produced no .elemod")
        elemod = arts[-1]

        # 5. out dir + manifest
        outdir = pathlib.Path(args.out)
        outdir.mkdir(parents=True, exist_ok=True)
        ename = f"dnvawt-{version}-custom-{tag}.elemod"
        dest = outdir / ename
        shutil.copy(elemod, dest)
        manifest = {
            "tool": "build-tables",
            "version": version,
            "tag": tag,
            "banks": [{display: source} for display, source in disp],
            "files": [bynum[n].name for n in pick],
            "stock": {"file": stock.name,
                      "sha256": hashlib.sha256(
                          stock.read_bytes()).hexdigest()},
            "timestamp": int(time.time()),
        }
        try:
            manifest["repo"] = subprocess.run(
                ["git", "rev-parse", "--short", "HEAD"], cwd=str(ROOT),
                capture_output=True, text=True).stdout.strip()
        except Exception:
            pass
        (outdir / (ename + ".json")).write_text(
            json.dumps(manifest, indent=2) + "\n")
        print(f"WROTE {dest}")

        # 6. host DSP tests gate the custom header
        env2 = dict(env)
        env2["WT_MODDIR"] = str(work)
        r = subprocess.run([sys.executable, str(ROOT / "tools" /
                                                "vawt_host_test.py")],
                           cwd=str(ROOT), env=env2,
                           capture_output=True, text=True)
        print(r.stdout[-1500:])
        if r.returncode != 0 or "ALL HOST TESTS PASSED" not in r.stdout:
            print(r.stderr[-2000:])
            fail("host DSP tests failed on the custom tables")

        # 7. optional full image
        if args.patch:
            for mid, mpath in (("core", EL / "mods" / "core-dn1"),
                               ("machines", EL / "mods" / "machines-dn1")):
                r = subprocess.run(
                    [sys.executable, "-m", "elekloader.sdk.build",
                     str(mpath), "--stock", str(stock)],
                    cwd=str(ROOT), env=env, capture_output=True, text=True)
                if r.returncode != 0:
                    print(r.stdout[-2000:] + r.stderr[-2000:])
                    fail(f"sdk.build {mid} failed")
            def newest(moddir, prefix):
                cands = sorted((moddir / "out").glob(prefix + "*.elemod"),
                               key=lambda p: p.stat().st_mtime)
                cands = [p for p in cands if "-os" not in p.name] or cands
                if not cands:
                    fail(f"no built {prefix} found")
                return cands[-1]
            core = newest(EL / "mods" / "core-dn1", "core-")
            mach = newest(EL / "mods" / "machines-dn1", "machines-")
            r = subprocess.run(
                [sys.executable, "-m", "elekloader.lint", str(dest),
                 "--stock", str(stock), "--with", str(core),
                 "--with", str(mach)],
                cwd=str(ROOT), env=env, capture_output=True, text=True)
            print(r.stdout[-1500:])
            if r.returncode != 0:
                print(r.stderr[-2000:])
                fail("lint failed on the custom elemod")
            syx = outdir / f"vawt-custom-{tag}.syx"
            r = subprocess.run(
                [sys.executable, "-m", "elekloader.patch",
                 "--stock", str(stock), "--mod", str(core),
                 "--mod", str(mach), "--mod", str(dest),
                 "--out", str(syx), "--version", version[:4]],
                cwd=str(ROOT), env=env, capture_output=True, text=True)
            print(r.stdout[-1500:])
            if r.returncode != 0:
                print(r.stderr[-2000:])
                fail("patch failed")
            print(f"WROTE {syx}")
        print("CUSTOM BUILD OK")
    finally:
        if args.keep_temp:
            print(f"temp kept at {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
