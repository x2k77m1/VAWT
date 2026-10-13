#!/usr/bin/env python3
"""Host-side functional test for the VAWT machine render (no firmware needed).

Compiles vawt/vawt.c natively with stubbed dnm_* helpers and drives
vawt_render through a C harness. Checks waveforms, knobs, levels and bounds.
"""
import math
import os
import pathlib
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
MOD = pathlib.Path(os.environ.get("WT_MODDIR", ROOT / "vawt"))

HARNESS = r"""
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "digitone-mk1/machines.h"

static int32_t test_knobs[16];
static int32_t test_pitch;

int32_t dnm_knob(int32_t voice, int32_t knob) {
    (void)voice;
    return test_knobs[knob & 15];
}
uint32_t dnm_phase_inc(int32_t pitch) {
    int32_t n = pitch >> 16;
    uint32_t f = (uint32_t)pitch & 0xffffu;
    double base;
    uint32_t a, b;
    if (n < 0) n = 0;
    if (n > 127) n = 127;
    base = 440.0 * pow(2.0, ((double)n - 69.0) / 12.0) / 48000.0 * 4294967296.0;
    a = (uint32_t)(base + 0.5);
    base = 440.0 * pow(2.0, ((double)(n + 1) - 69.0) / 12.0) / 48000.0 * 4294967296.0;
    b = (uint32_t)(base + 0.5);
    return a + (uint32_t)(((b - a) >> 8) * (f >> 8));
}
int32_t dnm_sin(uint32_t phase) {
    double s = sin(6.283185307179586 * (double)phase / 4294967296.0);
    return (int32_t)(s * 32767.0);
}

extern const struct dnm_machine vawt_machine;

static void run_blocks(int voice, int nblocks, int32_t pitch, int fresh_first, int32_t *out) {
    struct dnm_voice v;
    int b;
    memset(&v, 0, sizeof v);
    v.pitch = pitch;
    v.velocity = 100;
    v.track = 0;
    v.gate = 1;
    for (b = 0; b < nblocks; b++) {
        v.fresh = (b == 0 && fresh_first);
        vawt_machine.render(voice, out + 32 * b, &v);
    }
}

static int32_t peak32(const int32_t *p, int n) {
    int32_t m = 0, i;
    for (i = 0; i < n; i++) {
        int32_t a = p[i] < 0 ? -p[i] : p[i];
        if (a > m) m = a;
    }
    return m;
}

int main(void) {
    static int32_t buf[32 * 64];
    int i, n;
    /* knob defaults as 8.8, then per-test overrides from argv */
    return 0;
}
"""

# Full harness is generated per-test in Python instead: simpler to drive via
# a single C program that reads knob/pitch commands from stdin and writes
# raw int32 blocks to stdout. Keeps C small and test logic in Python.
DRIVER = r"""
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "digitone-mk1/machines.h"
static int32_t K[16];
static int32_t PITCH;
uint32_t vawt_ticks = 0; /* fuse last-seen clock; the test never advances it */
volatile int32_t fw_lfo_state[256]; /* zeros: DEST 0/MOD 0 = no VAWT match */
volatile int16_t fw_voice_params[1024];
volatile uint32_t fw_voice_pitch[8];
volatile int32_t fw_voice_len[8];
volatile uint32_t fw_gate_off;
volatile uint32_t fw_timeline;
volatile int32_t fw_transpose[5];
const char fw_str_empty[] = "";
/* Knob index -> sound slot (VAWT's map, same slots stock/dnm_knob use). */
static const int SLOTS[16] = {17,18,19,20,21,22,23,24,27,28,29,30,31,32,33,-1};
static void sync_knob(int k) {
    int s = SLOTS[k & 15];
    if (s >= 0) fw_voice_params[9 + 79 * 0 + s] = (int16_t)K[k & 15];
}
int32_t dnm_knob(int32_t voice, int32_t knob) { (void)voice; return K[knob & 15]; }
uint32_t dnm_phase_inc(int32_t pitch) {
    int32_t n = pitch >> 16; uint32_t f = (uint32_t)pitch & 0xffffu;
    double fa, fb; uint32_t a, b;
    if (n < 0) n = 0; if (n > 127) n = 127;
    fa = 440.0 * pow(2.0, ((double)n - 69.0) / 12.0) / 48000.0 * 4294967296.0;
    fb = 440.0 * pow(2.0, ((double)(n + 1) - 69.0) / 12.0) / 48000.0 * 4294967296.0;
    a = (uint32_t)(fa + 0.5); b = (uint32_t)(fb + 0.5);
    return a + (uint32_t)(((b - a) >> 8) * (f >> 8));
}
int32_t dnm_sin(uint32_t phase) {
    return (int32_t)(sin(6.283185307179586 * (double)phase / 4294967296.0) * 32767.0);
}
extern const struct dnm_machine vawt_machine;
int main(void) {
    static int32_t out[32];
    struct dnm_voice v;
    char line[256];
    int voice = 0, fresh = 1, i;
    memset(&v, 0, sizeof v);
    v.velocity = 100; v.track = 0; v.gate = 1;
    /* defaults */
    K[0]=0; K[1]=2304; K[2]=0; K[3]=512; K[4]=16128; K[5]=0; K[6]=0; K[7]=0;
    K[8]=16384; K[9]=0; K[10]=0; K[11]=32512; K[12]=32512; K[13]=16384; K[14]=0; K[15]=0;
    for (i = 0; i < 16; i++) sync_knob(i);
    PITCH = 69 << 16;
    while (fgets(line, sizeof line, stdin)) {
        int a, b, c, d;
        if (sscanf(line, "knob %d %d", &a, &b) == 2) { K[a & 15] = b; sync_knob(a); fresh = 1; }
        else if (sscanf(line, "pitch %d", &a) == 1) { PITCH = a; fresh = 1; }
        else if (sscanf(line, "lfo %d %d %d", &a, &b, &c) == 3) {
            int idx = a & 1;
            fw_lfo_state[10*idx+16] = (int32_t)b;
            fw_lfo_state[10*idx+17] = (int32_t)c;
            fresh = 1;
        }
        else if (sscanf(line, "run %d", &a) == 1) {
            for (i = 0; i < a; i++) {
                v.pitch = PITCH; v.fresh = fresh; fresh = 0;
                vawt_machine.render(voice, out, &v);
                fwrite(out, 4, 32, stdout);
            }
            fflush(stdout);
        }
        else if (!strncmp(line, "fresh", 5)) { fresh = 1; }
    }
    return 0;
}
"""


def build_driver(tmp):
    src = tmp / "driver.c"
    exe = tmp / "driver"
    src.write_text(DRIVER)
    inc = ROOT / "tools" / "elekloader" / "elekloader" / "sdk" / "include"
    cmd = ["cc", "-O1", "-o", str(exe), str(src), str(MOD / "vawt.c"),
           "-I", str(inc), "-I", str(MOD), "-lm"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-3000:])
        print(r.stderr[-3000:])
        raise SystemExit("host compile failed")
    return exe


class Synth:
    def __init__(self, exe):
        import subprocess as sp
        self.p = sp.Popen([str(exe)], stdin=sp.PIPE, stdout=sp.PIPE)
        self.cmds = []

    def send(self, s):
        self.p.stdin.write((s + "\n").encode())
        self.p.stdin.flush()

    def run(self, nblocks):
        self.send(f"run {nblocks}")
        raw = b""
        while len(raw) < nblocks * 32 * 4:
            chunk = self.p.stdout.read(nblocks * 32 * 4 - len(raw))
            if not chunk:
                raise RuntimeError("driver EOF")
            raw += chunk
        ns = nblocks * 32
        return list(struct.unpack(f"<{ns}i", raw))

    def close(self):
        self.p.stdin.close()
        self.p.wait()


def peak(x):
    return max(abs(s) for s in x)


def zc_rate(x):
    c = sum(1 for a, b in zip(x, x[1:]) if (a < 0) != (b < 0))
    return c / (len(x) - 1)


def zc_rate_interp(x):
    """Zero-crossing rate with linear-interpolated crossing positions."""
    pos = []
    for i, (a, b) in enumerate(zip(x, x[1:])):
        if (a < 0) != (b < 0):
            d = abs(a) + abs(b)
            pos.append(i + (abs(a) / d if d else 0.0))
    if len(pos) < 3:
        return zc_rate(x)
    return (len(pos) - 1) / (pos[-1] - pos[0])


def main():
    fails = []

    def check(name, cond, info=""):
        print(f"{'PASS' if cond else 'FAIL'} {name} {info}")
        if not cond:
            fails.append(name)

    with tempfile.TemporaryDirectory() as td:
        tmp = pathlib.Path(td)
        exe = build_driver(tmp)
        s = Synth(exe)
        G4 = 67 << 16  # MIDI 67 ≈ G4 (Digitone note convention: pitch = note<<16)

        # 1. SAW default: bounded, loud, saw-like (mean near 0, high peak)
        s.send("fresh")
        out = s.run(8)[32:]  # skip first block (phase reset transient)
        p = peak(out)
        check("saw-bounded", p <= 135000000, f"peak={p}")
        check("saw-loud", p > 60000000, f"peak={p}")

        # 2. SINE G4: fundamental only-ish, zero-crossing rate matches ~440Hz
        # (driver default pitch is A4 440Hz; long run beats quantisation)
        s.send("knob 0 1280")  # WAVE=5? no: WAVE index<<8: SINE=4 -> 4*256=1024
        s.send("knob 0 1024")
        s.send("fresh")
        out = s.run(48)[8 * 32:]
        zc = zc_rate(out)
        expect = 2 * 440.0 / 48000.0
        check("sine-pitch", abs(zc - expect) / expect < 0.03, f"zc={zc:.5f} expect={expect:.5f}")
        check("sine-bounded", peak(out) <= 135000000, f"peak={peak(out)}")

        # 3. FOLD adds harmonics: folded sine differs strongly from clean sine
        s.send("knob 0 1024")
        s.send("knob 5 0")
        s.send("fresh")
        clean = s.run(8)[32:96]
        s.send("knob 5 32512")
        s.send("fresh")
        folded = s.run(8)[32:96]
        diff = sum(abs(a - b) for a, b in zip(clean, folded)) / len(clean)
        check("fold-changes", diff > 10000000, f"meandiff={diff:.0f}")
        check("fold-bounded", peak(folded) <= 135000000, f"peak={peak(folded)}")
        s.send("knob 5 0")

        # 4. WT morph: WPOS 0 vs 127 differ on the FILE bank (default 0)
        s.send("knob 0 1280")  # WT = 5*256
        s.send("knob 14 0")  # FILE bank 0 (built-in vowels)
        s.send("knob 9 0")
        s.send("fresh")
        w0 = s.run(8)[32:96]
        s.send("knob 9 32512")
        s.send("fresh")
        w1 = s.run(8)[32:96]
        diff = sum(abs(a - b) for a, b in zip(w0, w1)) / len(w0)
        check("wt-morph", diff > 1000000, f"meandiff={diff:.0f}")

        # 4b. FILE selects the bank: another bank differs from bank 0,
        # out-of-range clamps to the last bank (bounded, alive). Bank
        # count comes from the header under test (factory: 24).
        import re as _re
        nbanks = int(_re.search(
            r"#define WT_BANKS (\d+)",
            (MOD / "wt_tables.h").read_text()).group(1))
        other = 2 if nbanks > 2 else (1 if nbanks > 1 else 0)
        s.send("knob 9 0")
        s.send("knob 14 0")
        s.send("fresh")
        b0 = s.run(8)[32:96]
        s.send(f"knob 14 {other * 256}")  # FILE bank N
        s.send("fresh")
        b2 = s.run(8)[32:96]
        if other:
            diff = sum(abs(a - b) for a, b in zip(b0, b2)) / len(b0)
            check("file-switches", diff > 1000000, f"meandiff={diff:.0f}")
        else:
            check("file-switches", True, "single bank: morph covers it")
        s.send("knob 14 32512")  # FILE 127 -> clamps to last bank
        s.send("fresh")
        bmax = s.run(8)[32:]
        check("file-clamp-bounded", peak(bmax) <= 135000000,
              f"peak={peak(bmax)}")
        check("file-clamp-live", peak(bmax) > 10000000, f"peak={peak(bmax)}")
        s.send("knob 14 0")

        # 4c. Removed WAVE slot (raw 7, beyond param 183's 0-6 range):
        # silent, reachable only via old locks.
        s.send("knob 0 1792")
        s.send("knob 14 0")
        s.send("fresh")
        blank = s.run(4)[32:]
        check("wave-blank-silent", peak(blank) < 1000000, f"peak={peak(blank)}")
        s.send("knob 0 0")

        # 5. PULSE width: mean shifts with PW
        s.send("knob 0 512")  # PULSE=2*256
        s.send("knob 8 0")
        s.send("fresh")
        p0 = s.run(8)[32:]
        m0 = sum(p0) / len(p0)
        s.send("knob 8 32512")
        s.send("fresh")
        p1 = s.run(8)[32:]
        m1 = sum(p1) / len(p1)
        check("pw-duty", abs(m1 - m0) > 20000000, f"mean {m0:.0f} -> {m1:.0f}")
        s.send("knob 8 16384")

        # 6. SUB adds level
        s.send("knob 0 0")  # SAW
        s.send("knob 6 0")
        s.send("fresh")
        a0 = s.run(8)[32:]
        s.send("knob 6 32512")
        s.send("fresh")
        a1 = s.run(8)[32:]
        check("sub-adds", peak(a1) >= peak(a0), f"{peak(a0)} -> {peak(a1)}")
        s.send("knob 6 0")

        # 7. NOISE: WAVE=NOI with NOIS up is non-periodic, zero mean-ish
        s.send("knob 0 1536")  # NOISE=6*256
        s.send("knob 7 32512")
        s.send("fresh")
        nz = s.run(16)[32:]
        check("noise-live", peak(nz) > 10000000, f"peak={peak(nz)}")
        check("noise-bounded", peak(nz) <= 135000000, f"peak={peak(nz)}")
        s.send("knob 7 0")

        # 8. TONE darkens SAW (fewer sharp edges)
        s.send("knob 0 0")
        s.send("knob 11 32512")
        s.send("fresh")
        bright = s.run(8)[32:]
        eb = sum(abs(a - b) for a, b in zip(bright, bright[1:]))
        s.send("knob 11 0")
        s.send("fresh")
        dark = s.run(8)[32:]
        ed = sum(abs(a - b) for a, b in zip(dark, dark[1:]))
        check("tone-darkens", ed < eb * 0.7, f"edge {eb:.0f} -> {ed:.0f}")
        s.send("knob 11 32512")

        # 9. CRUSH at full CCW: 4-bit quantize (s multiple of 4096);
        # at full CW (OFF): LSBs alive.
        s.send("knob 12 0")
        s.send("fresh")
        cr = s.run(4)[32:]
        step = 4096 << 12
        check("crush-quant", all(v % step == 0 for v in cr), f"step={step}")
        s.send("knob 12 32512")
        s.send("fresh")
        cl = s.run(4)[32:]
        check("crush-off", any(v % step != 0 for v in cl), "LSBs live")

        # 9b. Sample-rate reduction holds samples: slow sine, max crush,
        # filter open -> runs of >=8 identical samples; OFF -> none long.
        s.send("knob 0 1024")  # SINE
        s.send("pitch 2162688")  # 33<<16, A1
        s.send("knob 12 0")
        s.send("fresh")
        sr = s.run(8)[32:]

        def maxrun(x):
            best = cur = 1
            for a, b in zip(x, x[1:]):
                cur = cur + 1 if a == b else 1
                best = max(best, cur)
            return best

        check("srr-holds", maxrun(sr) >= 8, f"maxrun={maxrun(sr)}")
        s.send("knob 12 32512")
        s.send("fresh")
        so = s.run(8)[32:]
        check("srr-off", maxrun(so) < 8, f"maxrun={maxrun(so)}")
        s.send("pitch 4521984")  # back to 69<<16 default

        # 10. OCT shifts pitch 2x (long runs; skip transient).
        # Knob 3 serves param 182 since 0.1c (range 0-4): 512 = 0.
        s.send("knob 0 1024")  # SINE
        s.send("knob 3 512")  # OCT 0
        s.send("fresh")
        lo = s.run(48)[8 * 32:]
        s.send("knob 3 768")  # OCT +1
        s.send("fresh")
        hi = s.run(48)[8 * 32:]
        check("oct-doubles", abs(zc_rate_interp(hi) - 2 * zc_rate_interp(lo)) / zc_rate_interp(lo) < 0.02,
              f"{zc_rate_interp(lo):.5f} -> {zc_rate_interp(hi):.5f}")
        s.send("knob 3 1024")  # OCT +2
        s.send("fresh")
        hi2 = s.run(48)[8 * 32:]
        check("oct-quadruples", abs(zc_rate_interp(hi2) - 4 * zc_rate_interp(lo)) / zc_rate_interp(lo) < 0.02,
              f"{zc_rate_interp(lo):.5f} -> {zc_rate_interp(hi2):.5f}")
        s.send("knob 3 512")

        # 10b. SOCT rides along (knob 13, ranged mapping): -2/-1/0 stay
        # bounded with SUB running hot.
        s.send("knob 6 32512")  # SUB up
        for kval, kname in ((0, "-2"), (16384, "-1"), (25600, "0")):
            s.send(f"knob 13 {kval}")  # SOCT
            s.send("fresh")
            out = s.run(8)[32:]
            check(f"soct-{kname}-bounded", peak(out) <= 135000000, f"peak={peak(out)}")
        s.send("knob 13 16384")  # SOCT -1 (default)
        s.send("knob 6 0")

        # 10b. Alias energy: inharmonic power vs total. SAW/SQUARE/PULSE
        # (and SUB's square) run PolyBLEP; triangle/sine/wavetable don't
        # need it. Steady open-tone blocks, Hann-windowed FFT.
        import numpy as _np

        def alias_db(nblocks, wave, note, **knobs):
            s.send(f"knob 0 {wave}")
            for kk, vv in knobs.items():
                s.send(f"knob {kk} {vv}")
            s.send("knob 11 32512")  # tone open
            s.send("knob 12 32512")  # crush off
            s.send(f"pitch {note << 16}")
            s.send("fresh")
            out = s.run(nblocks)[16 * 32:]
            x = _np.array(out, dtype=_np.float64)
            x *= _np.hanning(len(x))
            f0 = 440.0 * 2.0 ** ((note - 69) / 12.0)
            sr = 48000.0
            mag = _np.abs(_np.fft.rfft(x))
            fr = _np.fft.rfftfreq(len(x), 1.0 / sr)
            harm = _np.zeros_like(fr, dtype=bool)
            k = 1
            while k * f0 < sr / 2:
                harm |= _np.abs(fr - k * f0) < f0 * 0.25
                k += 1
            total = _np.sum(mag ** 2)
            masked = mag[~harm & (fr > 100)] ** 2
            return 10 * math.log10(_np.sum(masked) / total)

        # SAW=0, SQUARE=1, SINE=1024, low=45 (110Hz) mid=69 high=93 (1760)
        # Thresholds from measured PolyBLEP residuals (quadratic BLEP is
        # theory-exact here: float reference matches to 0.0dB): mids prove
        # the correction is active, highs pin the worst whistle
        # components, all clustered 18-23kHz where filters bite hardest.
        check("saw-mid-alias", alias_db(256, 0, 69) < -40, "")
        check("saw-high-alias", alias_db(512, 0, 93) < -27, "")
        check("squ-mid-alias", alias_db(256, 256, 69) < -40, "")
        check("squ-high-alias", alias_db(512, 256, 93) < -28, "")
        check("sine-high-alias", alias_db(512, 1024, 93) < -60, "")
        s.send("pitch 4521984")  # back to 69<<16 default
        s.send("knob 0 1024")  # back to SINE for the sections below

        # 11. SYM symmetry: inaudible at middle (bit-exact bypass, same
        # peak as plain sine), strong even harmonics off middle, peaks
        # identical (tightened fold rail). SINE + open tone.
        import numpy as _np2

        def second_harm(symraw):
            s.send("knob 0 1024")
            s.send(f"knob 1 {symraw}")
            s.send("knob 5 0")
            s.send("knob 11 32512")
            s.send("knob 12 32512")
            s.send("pitch 4521984")
            s.send("fresh")
            out = s.run(64)[16 * 32:]
            x = _np2.array(out, dtype=_np2.float64)
            x *= _np2.hanning(len(x))
            mag = _np2.abs(_np2.fft.rfft(x))
            fr = _np2.fft.rfftfreq(len(x), 1.0 / 48000.0)
            fund = _np2.sum(mag[_np2.abs(fr - 440.0) < 20] ** 2)
            h2 = _np2.sum(mag[_np2.abs(fr - 880.0) < 20] ** 2)
            return (10 * math.log10(h2 / fund),
                    max(abs(vv) for vv in out))

        r_mid, p_mid = second_harm(2304)
        r_max, p_max = second_harm(4608)
        r_min, p_min = second_harm(0)
        check("sym-mid-clean", r_mid < -60, f"2nd {r_mid:.1f}dB")
        check("sym-max-evens", r_max > -20, f"2nd {r_max:.1f}dB")
        check("sym-min-evens", r_min > -20, f"2nd {r_min:.1f}dB")
        check("sym-peaks-equal", max(p_mid, p_max, p_min)
              - min(p_mid, p_max, p_min) <= p_mid // 1000,
              f"{p_mid} {p_max} {p_min}")
        s.send("knob 1 2304")

        # 12. System LFOs reach VAWT SYN slots (both LFOs). MOD is 8.8.
        # Slots: 20 OCT, 22 FOLD. Dest 127 = unused (>78).
        s.send("knob 0 1024")  # SINE
        s.send("knob 3 512")  # OCT 0
        s.send("knob 5 0")  # FOLD 0
        s.send("knob 11 32512")
        s.send("knob 12 32512")
        s.send("lfo 0 127 0")
        s.send("lfo 1 127 0")
        s.send("fresh")
        base_oct = s.run(48)[8 * 32:]
        f_base = zc_rate_interp(base_oct)
        # LFO1 -> OCT +1 (slot 20, +256 = +1 raw)
        s.send("lfo 0 20 256")
        s.send("fresh")
        hi_oct = s.run(48)[8 * 32:]
        f_hi = zc_rate_interp(hi_oct)
        check("lfo-oct-doubles", abs(f_hi - 2 * f_base) / f_base < 0.02,
              f"{f_base:.5f} -> {f_hi:.5f}")
        # LFO2 alone also reaches OCT
        s.send("lfo 0 127 0")
        s.send("lfo 1 20 256")
        s.send("fresh")
        hi2_oct = s.run(48)[8 * 32:]
        f_hi2 = zc_rate_interp(hi2_oct)
        check("lfo2-oct-doubles", abs(f_hi2 - 2 * f_base) / f_base < 0.02,
              f"{f_base:.5f} -> {f_hi2:.5f}")
        s.send("lfo 0 127 0")
        s.send("lfo 1 127 0")
        # LFO -> FOLD changes timbre (SINE clean vs modulated)
        s.send("knob 3 512")
        s.send("fresh")
        clean = s.run(8)[32:96]
        s.send("lfo 0 22 16384")
        s.send("fresh")
        folded = s.run(8)[32:96]
        diff = sum(abs(a - b) for a, b in zip(clean, folded)) / len(clean)
        check("lfo-fold-changes", diff > 10000000, f"meandiff={diff:.0f}")
        check("lfo-fold-bounded", peak(folded) <= 135000000,
              f"peak={peak(folded)}")
        # FILTER/AMP dests are ignored here (applied downstream): no change
        s.send("lfo 0 66 16384")
        s.send("fresh")
        ignored = s.run(8)[32:96]
        # fresh phases reset, so bit-exact when ignored (FOLD base 0 still)
        # note: clean was with lfo cleared; ignored should equal clean
        s.send("lfo 0 127 0")
        s.send("fresh")
        clean2 = s.run(8)[32:96]
        diff_ign = sum(abs(a - b) for a, b in zip(clean2, ignored)) / len(clean2)
        check("lfo-filter-ignored", diff_ign == 0, f"meandiff={diff_ign:.0f}")
        # Clamp: OCT max + push stays bounded and alive
        s.send("knob 0 1024")
        s.send("knob 3 1024")  # OCT +2 max
        s.send("lfo 0 20 512")
        s.send("fresh")
        clamped = s.run(8)[32:]
        check("lfo-clamp-bounded", peak(clamped) <= 135000000,
              f"peak={peak(clamped)}")
        check("lfo-clamp-live", peak(clamped) > 10000000,
              f"peak={peak(clamped)}")
        s.send("knob 3 512")
        s.send("lfo 0 127 0")
        s.send("lfo 1 127 0")
        s.close()

    print(f"\n{len(fails)} failures: {fails}" if fails else "\nALL HOST TESTS PASSED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
