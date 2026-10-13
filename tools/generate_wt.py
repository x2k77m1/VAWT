#!/usr/bin/env python3
"""Generate wavetable data for the VAWT Digitone machine.

Default banks 0-1 are two additive tables (A vocal-ish, B brightness sweep),
8 frames each,
256 samples, Q15 int16. Band-limited additive (sine phases, smooth morph),
each frame normalized to peak 30000.

Banks 2+ come from the user's .wav files (see WAV_RE below): 8 evenly
spread frames each, FFT-resynthesized to 256 samples keeping harmonics
1-120 (no aliasing), DC dropped, peak 30000.

Only SELECTION ships: mod RAM is 128KB shared and tables live in .run
(RAM-copied; the linker has no flash-resident section), so ~24 banks fit.
Rebuild with a different selection any time for another set.

Writes vawt/wt_tables.h: wt_banks[WT_BANKS][8][256] plus
wt_bank_names. Deterministic; needs numpy only when tools/OSW/ exists.
tools/OSW/ is local build input (gitignored): without it the header holds
just the two default additive tables.
"""
import math
import pathlib
import re
import struct
import wave

N = 256
PEAK = 30000
KEEP_HARM = 120

# Each frame: list of (harmonic, amplitude). Phase 0 (sine) for smooth morph.
#
# Bank A: an 8-vowel formant sweep. A saw-ish buzz with two moving formant
# bumps; the bumps' walk across frames is what talks.
def _formant(f1, f2, tilt=1.0):
    out = []
    for h in range(1, 25):
        a = tilt / h
        b = math.exp(-((h - f1) / 1.5) ** 2) + 0.7 * math.exp(-((h - f2) / 2.0) ** 2)
        out.append((h, a * (0.25 + b)))
    return out


BANK_A = [
    _formant(3, 9, 1.0),    # ah
    _formant(4, 11, 1.0),   # eh
    _formant(5, 8, 1.1),
    _formant(3, 7, 1.1),    # oh
    _formant(6, 12, 1.2),
    _formant(4, 9, 1.2),    # ih
    _formant(5, 13, 1.3),
    _formant(7, 10, 1.4),   # oo-bright
]

# Bank B: periodic glitch for atonal/percussion use. Deterministic (seeded),
# still single-cycle, deliberately non-musical next to bank A.
def _glitch():
    import random
    rnd = random.Random(0x51AB)
    return [
        [(1, 1.0), (3, 0.8), (5, 0.6), (7, 0.5), (9, 0.4), (11, 0.3), (13, 0.25)],
        [(1, 1.0), (3, -0.4), (5, 0.25), (7, -0.15), (9, 0.1)],
        [(h, 0.5) for h in range(1, 25)],
        [(1, 0.2), (6, 0.7), (8, 0.7)],
        [(h, rnd.uniform(-1.0, 1.0) / (1.0 + h * 0.1)) for h in range(1, 25)],
        [(h, 0.8 / (1 + (h - 1) * 0.15)) for h in range(1, 25, 2)],
        [(1, 1.0), (2, 0.9), (3, 0.2), (16, 0.3), (17, 0.3)],
        [(1, 0.8), (5, 0.6), (9, 0.5), (14, 0.4), (19, 0.3), (23, 0.25)],
    ]


BANK_B = _glitch()


def render_frame(partials):
    out = [0.0] * N
    for i in range(N):
        ph = 2.0 * math.pi * i / N
        s = 0.0
        for h, a in partials:
            s += a * math.sin(h * ph)
        out[i] = s
    peak = max(abs(s) for s in out) or 1.0
    return [int(round(s / peak * PEAK)) for s in out]


WAV_RE = re.compile(r"(?:.+\s)?(\d+)_(\d+)_(\d+)_(\d+)_16bit\.wav")
# Accepted files: mono 16-bit WAVs of equal single-cycle frames, named
# "<anything> <n>_<framelen>_<frames>_<rate>_16bit.wav" (n = table number
# for --pick and ordering). Frame count, length and rate may differ per
# file; junk names are skipped with a warning.


def read_user_bank(path):
    """One .wav -> 8 resynthesized 256-sample frames + short name."""
    m = WAV_RE.fullmatch(path.name)
    if not m:
        raise ValueError(f"bad wavetable name: {path.name}")
    _num, framelen, frames = int(m.group(1)), int(m.group(2)), int(m.group(3))
    with wave.open(str(path)) as w:
        if w.getnchannels() != 1 or w.getsampwidth() != 2:
            raise ValueError(f"{path.name}: need mono 16-bit")
        total = w.getnframes()
        raw = w.readframes(total)
    if total != frames * framelen:
        raise ValueError(
            f"{path.name}: {total} samples != {frames}x{framelen}")
    import numpy as np
    data = np.array(struct.unpack("<%dh" % total, raw), dtype=np.float64)
    picked = [round(i * (frames - 1) / 7) for i in range(8)]
    out = []
    for p in picked:
        frame = data[p * framelen:(p + 1) * framelen]
        spec = np.fft.rfft(frame)
        spec[0] = 0.0                       # drop DC (no offset through fold)
        if len(spec) > KEEP_HARM + 1:
            spec[KEEP_HARM + 1:] = 0.0      # band-limit: no aliasing at 256
        y = np.fft.irfft(spec, n=N)
        peak = max(abs(y.min()), abs(y.max())) or 1.0
        out.append([int(round(s / peak * PEAK)) for s in y])
    return out


def collect_flat(wav_dir, tag):
    """A flat folder of OSW .wavs -> [(name, frames)]. Names are TAG+number
    (tag: single A-Z0-9). Warns and skips non-matching files."""
    if not re.fullmatch(r"[A-Z0-9]", tag):
        raise SystemExit(f"bad --tag {tag!r}: single A-Z0-9")
    bynum = {}
    for f in sorted(wav_dir.iterdir(), key=lambda f: f.name):
        if f.suffix != ".wav":
            print(f"  SKIP {f.name} (not a .wav)")
            continue
        m = WAV_RE.fullmatch(f.name)
        if not m:
            print(f"  SKIP {f.name} (not an OSW wavetable)")
            continue
        n = int(m.group(1))
        if n in bynum:
            raise SystemExit(f"duplicate table number {n} in {wav_dir}")
        bynum[n] = f
    return bynum


def collect_user_banks(osw_dir):
    """A two-batch layout (kept for the factory header): sibling folders
    map to letters alphabetically. Only SELECTION ships (mod-RAM ceiling);
    edit it to change the set. End users should use build-tables.py with
    a flat folder instead."""
    import numpy  # noqa: F401  (fail fast with a clear error if missing)
    banks = []
    for letter, sub in zip("ABCDEFGHIJKLMNOPQRSTUVWXYZ",
                           sorted(p for p in osw_dir.iterdir() if p.is_dir())):
        bynum = {}
        for f in sub.iterdir():
            if f.suffix != ".wav":
                continue
            m = WAV_RE.fullmatch(f.name)
            if not m:
                print(f"  SKIP {f.name} (not an OSW wavetable)")
                continue
            bynum[int(m.group(1))] = f
        for num in sorted(bynum):
            name = f"{letter}{num:02d}"
            if name not in SELECTION:
                continue
            f = bynum[num]
            banks.append((name, read_user_bank(f)))
            print(f"  bank {banks[-1][0]} <- {f.parent.name}/{f.name}")
    missing = [nm for nm in SELECTION if nm not in [b[0] for b in banks]]
    if missing:
        raise SystemExit(f"SELECTION not found in tools/OSW/: {missing}")
    return banks


# Curated resident set: 24 plain slots (no slot is privileged). One-line
# change + rerun for another set; kept indices never move as long as
# entries are only appended.
SELECTION = [f"A{n:02d}" for n in range(1, 12)] + \
            [f"B{n:02d}" for n in range(1, 12)]


def emit_tables(banks):
    lines = [f"static const int16_t wt_banks[{len(banks)}][8][256] = {{"]
    for _name, frames in banks:
        assert len(frames) == 8
        lines.append("    {")
        for samp in frames:
            assert len(samp) == N and all(-32768 <= s <= 32767 for s in samp)
            lines.append("        {" + ", ".join(str(s) for s in samp) + "},")
        lines.append("    },")
    lines.append("};")
    return "\n".join(lines)


def emit_names(names):
    lines = [f"static const char *const wt_bank_names[{len(names)}] = {{"]
    for nm in names:
        assert len(nm) <= 8, nm
        lines.append(f'    "{nm}",')
    lines.append("};")
    return "\n".join(lines)


def builtin_banks():
    """The two additive tables as [(source, frames)]. Just another
    selection entry: no slot is privileged, all 24 are replaceable."""
    return [("built-in vowels",
             [render_frame(p) for p in BANK_A]),
            ("built-in glitch",
             [render_frame(p) for p in BANK_B])]


MAX_BANKS = 24  # mod-RAM ceiling for resident banks


def build_header(out_path, banks):
    """Write the header for [(source, frames)] in order. Returns the
    [(display, source)] map. Display names are positional (WT1..WTN).
    Indices are append-only: kept banks never move when the selection
    grows. At least 1 bank; at most MAX_BANKS."""
    if not banks:
        raise SystemExit("no banks: nothing to write")
    if len(banks) > MAX_BANKS:
        raise SystemExit(f"{len(banks)} banks > RAM ceiling "
                         f"MAX_BANKS={MAX_BANKS}")
    disp = [(f"WT{i + 1}", src) for i, (src, _fr) in enumerate(banks)]
    frames = [fr for _src, fr in banks]
    hdr = (
        "/* Generated by tools/generate_wt.py - do not edit by hand. */\n"
        f"/* {len(frames)} wavetable banks,"
        " 8 frames x 256 samples, Q15. */\n"
        "#pragma once\n"
        "#include <stdint.h>\n\n"
        f"#define WT_BANKS {len(frames)}\n\n"
        + emit_tables(list(zip([d for d, _s in disp], frames)))
        + "\n\n"
        + emit_names([d for d, _s in disp])
        + "\n"
    )
    out_path.write_text(hdr)
    print(f"WROTE {out_path} ({len(hdr)} bytes, {len(frames)} banks)")
    return disp


def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    out = root / "vawt" / "wt_tables.h"
    user_banks = []
    osw = root / "tools" / "OSW"
    if osw.is_dir():
        try:
            user_banks = collect_user_banks(osw)
        except ImportError:
            print("NOTE: numpy missing, tools/OSW/ skipped "
                  "(pip install numpy for user banks)")
    else:
        print("NOTE: no tools/OSW/, header holds built-ins only")
    build_header(out, builtin_banks() +
                 [(nm, fr) for nm, fr in user_banks])


if __name__ == "__main__":
    main()
