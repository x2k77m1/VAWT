/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VAWT: a virtual-analogue + wavetable machine for the Digitone mk1
 * (the machines companion, digitone-mk1/machines.h).
 *
 * One oscillator (WAVE) with a detuned twin (UNIS/DET), a rounded-square
 * sub (SUB/SOCT), white noise (NOIS), a wavefolder (FOLD), a crusher
 * (CRSH: bit depth + sample rate reduction) and a one-pole lowpass
 * (TONE), octave (OCT) and analog drift (DRFT).
 * Wavetable position (WPOS) scans the WT banks; pulse width (PW) shapes
 * SQUARE/PULSE. Runs through the Digitone's own filter, mixer, effects and
 * AMP envelope like FM.
 *
 * Knobs (SYN1 then SYN2), all with stock ranges — except knob D: its
 * stock parameter (110) only spans 0-2, so 0.1c puts our own parameter
 * 182 there (range 0-4) via a page-constructor patch, editing the same
 * sound slot 20:
 *   A  WAVE  0-7: SAW, SQUARE, PULSE, TRI, SINE, WT, NOISE, blank
 *   (WT plays the TABL bank; position 8 is silent)
 *   B  SYM   0-18: wavefolder symmetry, middle = symmetric folds. An
 *       offset into the folder for even harmonics and lopsided folds;
 *       pairs with FOLD. Middle is bit-exact bypass.
 *   C  UNIS  0-35: second-oscillator mix 0-100%
 *   D  OCT   0-4 (param 182): octave -2..+2
 *   E  DET   37-89: second-oscillator detune, -52..+52 cents (bipolar, mid 63)
 *   F  FOLD  0-127: wavefolder 0-100%
 *   G  SUB   0-127: sub level 0-100%
 *   H  NOIS  0-127: noise level 0-100%
 *   I  PW    0-127: pulse width 5-95% (SQUARE/PULSE)
 *   J  WPOS  0-127: wavetable position 0-100% (WT mode)
 *   K  DRFT  0-127: analog drift 0-100%
 *   L  TONE  0-127: lowpass 0-100% (127 = open)
 *   M  CRSH  0-127: crusher, OFF at full clockwise, then 12-bit/18kHz
 *             down to 2-bit/2kHz as it turns down
 *   N  SOCT  0-127: sub octave -2, -1, 0
 *   O  TABL  0-23: wavetable bank (24 bundled tables, all replaceable);
 *             P unused.
 *
 * PW shapes SQUARE/PULSE only; WPOS scans the TABL bank's 8 frames only
 * (like FM ratios that do nothing in some algorithms). Everything else
 * applies to every WAVE.
 * Cost control (0.1w, after 0.1v hardware freezes): the render inlines
 * its PolyBLEP residual (no per-sample calls), reads voice slots
 * directly (no per-knob helper calls), and skips the drift sine, the
 * noise LFSR and the fold gain as bit-exact identities when their
 * amounts are zero — so a default patch costs a fraction of a full one.
 * The only per-sample divides are the BLEP residuals, and only for
 * samples adjacent to an edge (a few percent at normal pitches).
 */
#include "digitone-mk1/machines.h"
#include "wt_tables.h"

static uint32_t vawt_p1[8], vawt_p2[8], vawt_ps[8];
static uint32_t vawt_drift[8];
static uint32_t vawt_noise[8];
static int32_t vawt_lp[8];
static uint32_t vawt_hold_acc[8];
static int32_t vawt_hold_val[8];

/* LFO support: the two system LFOs modulate VAWT's SYN slots like FM.
 * dnm_knob() reads FW_VOICE_PARAM (base + locks, no LFO); the firmware's
 * LFO engine (0x400feaee) writes DEST (sound slot 0-78) and MOD (8.8
 * signed) per voice into fw_lfo_state, then applies them to a working
 * copy for FM. VAWT bypasses FM, so it must add MOD itself when DEST
 * matches one of its slots. FILTER/AMP dests are applied downstream
 * (filter/mix) and are intentionally ignored here to avoid double-apply.
 * FM voices never enter vawt_render, so FM LFO keeps working untouched
 * (T1 VAWT + T2-T4 FM is the safe mix).
 * Layout (verified in firmware): LFO1 DEST/MOD = voice*0x50+0x40/0x44,
 * LFO2 = voice*0x50+0x68/0x6C, i.e. FW_LFO(v,i)[16]/[17] with
 * FW_LFO(v,i)=base+20*v+10*i (ints). Only SYN slots are applied. */
static const uint8_t vawt_lfo_slots[15] = {
    17, 18, 19, 20, 21, 22, 23, 24, 27, 28, 29, 30, 31, 32, 33,
};
/* Clamp ranges in 8.8: stock ranges, except WAVE (param 183: 0-6) and
 * OCT (param 182: 0-4). DET raw 37-89 (mid 63); rest 0-127. */
static const int32_t vawt_lfo_min[15] = {
    0, 0, 0, 0, 9472, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
static const int32_t vawt_lfo_max[15] = {
    1536, 4608, 8960, 1024, 22784, 32512, 32512, 32512,
    32512, 32512, 32512, 32512, 32512, 32512, 32512,
};

static int32_t vawt_add_lfo(int32_t slot, int32_t val, int32_t mn, int32_t mx,
                             int32_t d0, int32_t m0, int32_t d1, int32_t m1)
{
    if (d0 == slot) {
        val += m0;
        if (val < mn)
            val = mn;
        else if (val > mx)
            val = mx;
    }
    if (d1 == slot) {
        val += m1;
        if (val < mn)
            val = mn;
        else if (val > mx)
            val = mx;
    }
    return val;
}

/* Polyphony fuse: VAWT is monophonic — at most VAWT_MAX_VOICES (=1)
 * VAWT note renders at once; the rest write silence until the slot
 * frees. The render runs at interrupt level every block, and even 4
 * simultaneous full-patch VAWT notes (twin + sub + fold + SYM off
 * middle, 0.1v) wedged the hardware (UI dead, gates stuck droning,
 * reboot needed; FM unaffected since the DSP renders it). Our code has
 * no unbounded loops (fold caps at 8, all tables clamped, BLEP divides
 * only adjacent to an edge), so the mechanism is block overrun
 * starving the UI task: a full patch costs ~2.6x a default one on the
 * host, so 4 full voices exceed the proven-stable 4 default SAWs and
 * even 2 full voices leave no margin once the UI (turns, overlay,
 * sequencer) loads the block. 0.1w also inlines BLEP, reads slots
 * directly and skips drift/noise/gain identities, but the voice count
 * is what guarantees the worst case: 1 full voice stays inside the
 * 667 us block with margin. Dropping excess notes is far better than
 * bricking the box. SINE takes ~3k instructions a voice a block
 * (ADAPTING.md); VAWT is several times that.
 * Bookkeeping uses only the dnm_voice struct (no firmware reads, so the
 * host test still builds): a release clears its slot on the very next
 * render call, so sequencer trigs free immediately. Birth order comes
 * from a render-side clock (unique per call, so simultaneous notes still
 * rank); the last-seen stamp uses the UI tick counter, and vawt_reap on
 * the UI tick clears voices the companion stopped sending (envelope
 * ended, left VAWT mid-note) — the tick runs even with zero render
 * callers, where a render-side clock would freeze with stale counts.
 * Latest notes keep the slot (mono needs newest-wins: holding one note
 * must not silence the next); a freed slot resumes a still-held note
 * from frozen phases. */
#define VAWT_MAX_VOICES 1
#define VAWT_TICK_REAP 120u
uint8_t vawt_sounding[8];
uint32_t vawt_birth[8];
uint32_t vawt_seen[8];
static uint32_t vawt_clock;
extern uint32_t vawt_ticks;

/* UI-tick backstop for the fuse: clear voices unobserved for
 * VAWT_TICK_REAP ticks. A live voice stamps every block (sub-tick), so
 * only truly uncalled voices age out. Called from vawt_tick, which runs
 * even with zero render callers. */
void vawt_reap(void)
{
    int32_t w;
    for (w = 0; w < 8; w++) {
        if (vawt_sounding[w] &&
            (int32_t)(vawt_ticks - vawt_seen[w]) > (int32_t)VAWT_TICK_REAP) {
            vawt_sounding[w] = 0;
            vawt_birth[w] = 0;
        }
    }
}

static void put_int(char *buf, int32_t v)
{
    char t[8];
    int32_t n = 0;
    if (v < 0) {
        *buf++ = '-';
        v = -v;
    }
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 7);
    while (n)
        *buf++ = t[--n];
    *buf = 0;
}

static void put_pct(char *buf, int32_t p)
{
    int32_t n = 0;
    char t[8];
    if (p < 0)
        p = 0;
    if (p > 100)
        p = 100;
    do {
        t[n++] = (char)('0' + p % 10);
        p /= 10;
    } while (p && n < 7);
    while (n)
        *buf++ = t[--n];
    *buf++ = '%';
    *buf = 0;
}

void vawt_wave_text(int32_t value, char *buf)
{
    static const char names[8][5] = {
        "SAW", "SQU", "PLS", "TRI", "SIN", "WT", "NOI", "---",
    };
    int32_t w = value >> 8;
    const char *s;
    int32_t i;
    if (w < 0)
        w = 0;
    if (w > 7)
        w = 7;
    s = names[w];
    for (i = 0; s[i]; i++)
        buf[i] = s[i];
    buf[i] = 0;
}

void vawt_unis_text(int32_t value, char *buf)
{
    put_pct(buf, (value >> 8) * 100 / 35);
}

void vawt_sym_text(int32_t value, char *buf)
{
    int32_t d = ((value >> 8) - 9) * 100 / 9;
    if (d > 0)
        *buf++ = '+';
    put_int(buf, d);
}

void vawt_oct_text(int32_t value, char *buf)
{
    int32_t o = (value >> 8) - 2;
    if (o < -2)
        o = -2;
    if (o > 2)
        o = 2;
    if (o > 0)
        *buf++ = '+';
    put_int(buf, o);
}

void vawt_det_text(int32_t value, char *buf)
{
    int32_t d = ((value >> 8) - 63) * 2;
    if (d > 0)
        *buf++ = '+';
    put_int(buf, d);
}

void vawt_pct127_text(int32_t value, char *buf)
{
    put_pct(buf, (value >> 8) * 100 / 127);
}

void vawt_pw_text(int32_t value, char *buf)
{
    put_pct(buf, 5 + (value >> 8) * 90 / 127);
}

void vawt_crush_text(int32_t value, char *buf)
{
    int32_t r = value >> 8, c, bits, n = 0, i;
    char t[8];
    if (r >= 127) {
        buf[0] = 'O';
        buf[1] = 'F';
        buf[2] = 'F';
        buf[3] = 0;
        return;
    }
    c = 127 - r;
    bits = 12 - ((c - 1) * 10) / 126;
    do {
        t[n++] = (char)('0' + bits % 10);
        bits /= 10;
    } while (bits && n < 7);
    for (i = 0; n; i++)
        buf[i] = t[--n];
    buf[i++] = 'b';
    buf[i] = 0;
}

void vawt_soct_text(int32_t value, char *buf)
{
    int32_t r = value >> 8, s = r < 43 ? -2 : r < 86 ? -1 : 0;
    put_int(buf, s);
}

void vawt_file_text(int32_t value, char *buf)
{
    int32_t b = value >> 8, i;
    const char *s;
    if (b < 0)
        b = 0;
    if (b > WT_BANKS - 1)
        b = WT_BANKS - 1;
    s = wt_bank_names[b];
    for (i = 0; s[i]; i++)
        buf[i] = s[i];
    buf[i] = 0;
}

const struct dnm_knob vawt_knobs[DNM_KNOBS] = {
    /* look is the knob's own stock id (native graphic and value boxes) except
     * where the stock value text would mislead: SYM/UNIS/OCT keep a plain
     * knob (look 0), since the ratio texts mean nothing as symmetry/mix/
     * octave. WAVE's box reads 1-8 for SAW..NOI, which the README maps. */
    [0] = { "WAVE", vawt_wave_text, 107 },
    [1] = { "SYM", vawt_sym_text, 0 },
    [2] = { "UNIS", vawt_unis_text, 0 },
    [3] = { "OCT", vawt_oct_text, 0 },
    [4] = { "DET", vawt_det_text, 111 },
    [5] = { "FOLD", vawt_pct127_text, 112 },
    [6] = { "SUB", vawt_pct127_text, 113 },
    [7] = { "NOIS", vawt_pct127_text, 114 },
    [8] = { "PW", vawt_pw_text, 119 },
    [9] = { "WPOS", vawt_pct127_text, 120 },
    [10] = { "DRFT", vawt_pct127_text, 121 },
    [11] = { "TONE", vawt_pct127_text, 122 },
    [12] = { "CRSH", vawt_crush_text, 123 },
    [13] = { "SOCT", vawt_soct_text, 124 },
    [14] = { "TABL", vawt_file_text, 0 },
};

/* OCT's own parameter (core_params id 182): knob D's stock parameter
 * (110) only spans raw 0-2, so a -2..+2 octave cannot live on it. This
 * record clones 110's shape (group, flags, CC, turn character) with a
 * 0-4 range on the same sound slot 20; a page-constructor patch puts it
 * on SYN1-D in place of 110. FM keeps 110 untouched. */
const struct core_param vawt_oct_param = {
    182, 0, 20, 0, 1024, 512, 1, 1048624, 203, 160, 3584,
    "Octave", "SYN", "OCT", vawt_oct_text, fw_str_empty, 0,
};

/* WAVE's own parameter (core_params id 183): knob A's stock parameter
 * (107) spans raw 0-7, one more than the seven VAWT waves (0-6: the last
 * slot would sit silent showing "---"). Same clone shape as 182, 0-6 on
 * the same sound slot 17; the page-constructor patch asks for it on VAWT
 * tracks only (FM keeps stock 107). No MIDI CC (-1): cloning 107's would
 * need its record, and sharing OCT's would cross-talk. LFOs target sound
 * slots, so WAVE/OCT follow the slot (host-tested, hardware pending). */
const struct core_param vawt_wave_param = {
    183, 0, 17, 0, 1536, 0, 1, -1, 203, 160, 3584,
    "Wave", "SYN", "WAV", vawt_wave_text, fw_str_empty, 107,
};

/* One wavetable sample: bilinear in phase and frame. tbl is 8x256 Q15.
 * The lerps split their shifts so (diff * frac) never overflows 32 bits.
 * Split for inlining: wt_phase lerps one frame in phase, wt_sample_fr
 * adds the frame lerp. Frame pointers / f1 / ff are hoisted once per
 * block in the render; ff==0 skips the second frame as an identity. */
static __attribute__((always_inline)) inline int32_t wt_phase(const int16_t *fr, uint32_t p)
{
    uint32_t pi = p >> 24;
    uint32_t pi1 = (pi + 1u) & 255u;
    int32_t pf4 = (int32_t)((p >> 12) & 0xfffu); /* == (((p>>8)&0xffff)>>4) */
    int32_t a0 = fr[pi], a1 = fr[pi1];
    return a0 + (int32_t)((((a1 - a0) >> 4) * pf4) >> 8);
}

static __attribute__((always_inline)) inline int32_t wt_sample_fr(const int16_t *fa, const int16_t *fb, uint32_t p, int32_t ff4, int32_t use_ff)
{
    int32_t s0 = wt_phase(fa, p);
    if (!use_ff)
        return s0;
    int32_t s1 = wt_phase(fb, p);
    return s0 + (int32_t)((((s1 - s0) >> 4) * ff4) >> 8);
}

static int32_t wt_sample(const int16_t tbl[8][256], uint32_t p, int32_t f0, int32_t ff)
{
    int32_t f1 = f0 < 7 ? f0 + 1 : 7;
    return wt_sample_fr(tbl[f0], tbl[f1], p, ff >> 4, ff != 0);
}

static __attribute__((always_inline)) inline uint32_t lfsr_next(uint32_t *s)
{
    uint32_t x = *s;
    if (!x)
        x = 0x1234567u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/* PolyBLEP residual for a full-scale falling edge at phase 0, in sample
 * units (subtract it at the edge; negate for a rising edge). Phase t and
 * rate dt are Q16 cycles; dt >= 11 always holds (lowest step), so the
 * division is safe, and t/dt < 1 keeps every intermediate in 32 bits.
 * Zero away from the edge: the division runs only for samples adjacent
 * to one (a few percent of samples at normal pitches), so the average
 * cost is a couple of compares. Always inlined: a call per sample per
 * edge (up to 6 with twin + sub) cost more than the compares on
 * ColdFire, and kept 4 full-patch voices over the block budget. */
static __attribute__((always_inline)) inline int32_t blep_fall(uint32_t t, uint32_t dt)
{
    if (t < dt) {
        uint32_t u = (t << 16) / dt;
        return ((int32_t)(u << 1) - (int32_t)((u * u) >> 16) - 65536) >> 1;
    }
    if (t > (uint32_t)(0x10000u - dt)) {
        uint32_t q = ((0x10000u - t) << 16) / dt;
        return ((int32_t)((q * q) >> 16) - (int32_t)(q << 1) + 65536) >> 1;
    }
    return 0;
}

static void vawt_render(int32_t voice, int32_t *block, const struct dnm_voice *v)
{
    int32_t lfo0_dest, lfo0_mod, lfo1_dest, lfo1_mod;
    int32_t wv, symraw, unraw, octv, det, fold, sublvl, noilvl;
    int32_t pw, wtpos, driftamt, tone, crush, soctr, bank;
    voice &= 7;
    /* Knob bases + LFO, read once per block. Bases come straight from
     * the voice's sound slots (no dnm_knob helper calls: 15 calls a
     * block a voice of pure overhead). The slots are VAWT's own map
     * (17-24, 27-33, hardware-proven by TABL-turn isolation and DET
     * lock photos); WAVE/OCT clones (params 183/182) edit the same
     * slots 17/20 as stock 107/110, so the values match dnm_knob
     * exactly. FW_LFO(v,i) already encodes the firmware's
     * LFO2-beyond-voice layout (+0x68). */
    lfo0_dest = (int32_t)FW_LFO(voice, 0)[FW_LFO_DEST];
    lfo0_mod = (int32_t)FW_LFO(voice, 0)[FW_LFO_MOD];
    lfo1_dest = (int32_t)FW_LFO(voice, 1)[FW_LFO_DEST];
    lfo1_mod = (int32_t)FW_LFO(voice, 1)[FW_LFO_MOD];
    wv = vawt_add_lfo(17, (int32_t)FW_VOICE_PARAM(voice, 17), 0, 1536,
                      lfo0_dest, lfo0_mod, lfo1_dest, lfo1_mod) >> 8;
    symraw = vawt_add_lfo(18, (int32_t)FW_VOICE_PARAM(voice, 18), 0, 4608,
                          lfo0_dest, lfo0_mod, lfo1_dest, lfo1_mod) >> 8;
    unraw = vawt_add_lfo(19, (int32_t)FW_VOICE_PARAM(voice, 19), 0, 8960,
                         lfo0_dest, lfo0_mod, lfo1_dest, lfo1_mod) >> 8;
    octv = vawt_add_lfo(20, (int32_t)FW_VOICE_PARAM(voice, 20), 0, 1024,
                        lfo0_dest, lfo0_mod, lfo1_dest, lfo1_mod) >> 8;
    det = ((vawt_add_lfo(21, (int32_t)FW_VOICE_PARAM(voice, 21), 9472, 22784,
                         lfo0_dest, lfo0_mod, lfo1_dest, lfo1_mod) >> 8) - 63) * 2;
    fold = vawt_add_lfo(22, (int32_t)FW_VOICE_PARAM(voice, 22), 0, 32512,
                        lfo0_dest, lfo0_mod, lfo1_dest, lfo1_mod) >> 8;
    sublvl = vawt_add_lfo(23, (int32_t)FW_VOICE_PARAM(voice, 23), 0, 32512,
                          lfo0_dest, lfo0_mod, lfo1_dest, lfo1_mod) >> 8;
    noilvl = vawt_add_lfo(24, (int32_t)FW_VOICE_PARAM(voice, 24), 0, 32512,
                          lfo0_dest, lfo0_mod, lfo1_dest, lfo1_mod) >> 8;
    pw = vawt_add_lfo(27, (int32_t)FW_VOICE_PARAM(voice, 27), 0, 32512,
                      lfo0_dest, lfo0_mod, lfo1_dest, lfo1_mod) >> 8;
    wtpos = vawt_add_lfo(28, (int32_t)FW_VOICE_PARAM(voice, 28), 0, 32512,
                         lfo0_dest, lfo0_mod, lfo1_dest, lfo1_mod) >> 8;
    driftamt = vawt_add_lfo(29, (int32_t)FW_VOICE_PARAM(voice, 29), 0, 32512,
                            lfo0_dest, lfo0_mod, lfo1_dest, lfo1_mod) >> 8;
    tone = vawt_add_lfo(30, (int32_t)FW_VOICE_PARAM(voice, 30), 0, 32512,
                        lfo0_dest, lfo0_mod, lfo1_dest, lfo1_mod) >> 8;
    crush = vawt_add_lfo(31, (int32_t)FW_VOICE_PARAM(voice, 31), 0, 32512,
                         lfo0_dest, lfo0_mod, lfo1_dest, lfo1_mod) >> 8;
    soctr = vawt_add_lfo(32, (int32_t)FW_VOICE_PARAM(voice, 32), 0, 32512,
                         lfo0_dest, lfo0_mod, lfo1_dest, lfo1_mod) >> 8;
    bank = vawt_add_lfo(33, (int32_t)FW_VOICE_PARAM(voice, 33), 0, 32512,
                        lfo0_dest, lfo0_mod, lfo1_dest, lfo1_mod) >> 8;
    int32_t wave, mix, soct, base, driftc, inc1, inc2, incs;
    int32_t oct;
    int32_t gain, lpc, cmask, rate, f0, ff, wpos;
    /* Zero-cost skips, all bit-exact identities: a zero mix adds s2*0, a
     * zero sub adds 0, crusher OFF masks nothing and takes every sample
     * (its accumulator nets zero), and an open tone (lpc 256) is y = s
     * with the state following. Phases still advance so turning a knob
     * mid-note stays continuous. This is most of the bill on a default
     * patch (single SAW, no sub/noise/fold, open tone, crusher off). */
    int32_t use_s2, use_sub, do_crush, do_lp;
    uint32_t pwth, step;
    uint32_t p1, p2, ps;
    int32_t i;
    int32_t w, rank;

    /* Fuse bookkeeping first (cheap). Sounding marks come only from our
     * own render calls (FM voices never enter here); releases clear
     * immediately so short sequencer trigs free their slots at once. */
    vawt_seen[voice] = vawt_ticks;
    if (v->fresh) {
        vawt_sounding[voice] = 1;
        vawt_birth[voice] = ++vawt_clock;
    } else if (!v->gate) {
        vawt_sounding[voice] = 0;
    } else if (!vawt_sounding[voice]) {
        vawt_sounding[voice] = 1;
        vawt_birth[voice] = ++vawt_clock;
    }
    rank = 0;
    for (w = 0; w < 8; w++) {
        if (vawt_sounding[w] &&
            (int32_t)(vawt_birth[w] - vawt_birth[voice]) > 0)
            rank++;
    }
    if (rank >= VAWT_MAX_VOICES) {
        for (i = 0; i < 32; i++)
            block[i] = 0;
        return;
    }

    wave = wv < 0 ? 0 : wv > 7 ? 7 : wv;
    if (bank < 0)
        bank = 0;
    if (bank > WT_BANKS - 1)
        bank = WT_BANKS - 1;
    mix = (unraw * 29) >> 3;
    if (mix < 0)
        mix = 0;
    if (mix > 127)
        mix = 127;
    use_s2 = mix != 0;
    use_sub = sublvl != 0;
    do_crush = crush < 127;
    do_lp = tone < 127;
    oct = octv - 2;
    if (oct < -2)
        oct = -2;
    if (oct > 2)
        oct = 2;
    soct = soctr < 43 ? -2 : soctr < 86 ? -1 : 0;

    int32_t sym = (symraw - 9) * 910;   /* +-8190: fold offset, 0 bypass */
    int32_t need_nz = (wave == 6) || (noilvl != 0);
    base = v->pitch + (oct * 12 << 16);
    if (driftamt) {
        vawt_drift[voice] += 371200u;
        driftc = (int32_t)(((dnm_sin(vawt_drift[voice]) >> 8) * driftamt) >> 10);
    } else {
        driftc = 0;         /* bit-exact: sin * 0 is 0; phase stays frozen
                             * so enabling drift mid-note stays continuous */
    }
    inc1 = (int32_t)dnm_phase_inc(base + driftc * 655);
    inc2 = (int32_t)dnm_phase_inc(base + (det + driftc) * 655);
    incs = (int32_t)dnm_phase_inc(base + (soct * 12 << 16));

    gain = 128 + fold * 3;
    lpc = tone >= 127 ? 256 : 8 + (tone << 1) - (tone >> 4);
    if (crush >= 127) {
        cmask = 0;                        /* OFF at full clockwise */
        step = 1u << 16;
    } else {
        int32_t c = 127 - crush, bits, drop;
        bits = 12 - ((c - 1) * 10) / 126; /* 12 down to 2 */
        drop = 16 - bits;
        cmask = (int32_t)((1u << (uint32_t)drop) - 1u);
        rate = 18000 - ((c - 1) * 16000) / 126;   /* 18k down to 2k */
        step = (uint32_t)(((uint32_t)rate * 1393u) >> 10);  /* rate/48k in Q16 */
    }
    pwth = (uint32_t)((5 + (pw * 90) / 127) * 42949673u);
    if (wave == 1)
        pwth = 0x80000000u;             /* SQUARE is pulse at 50% */
    wpos = wtpos * 7;
    f0 = wpos / 127;
    ff = (wpos % 127) * 516;
    if (f0 < 0)
        f0 = 0;
    if (f0 > 7)
        f0 = 7;
    /* WT frame hoists: base pointers, f1 and frame fraction once per
     * block. Bit-exact: same lerps, second frame skipped only when
     * ff==0 (identity). */
    int32_t f1 = f0 < 7 ? f0 + 1 : 7;
    const int16_t *wa0 = wt_banks[bank][f0];
    const int16_t *wa1 = wt_banks[bank][f1];
    int32_t ff4 = ff >> 4;
    int32_t use_ff = ff != 0;

    if (v->fresh) {
        vawt_p1[voice] = vawt_p2[voice] = vawt_ps[voice] = 0;
        vawt_hold_acc[voice] = 1u << 16;  /* take a sample at once */
    }
    if (!vawt_noise[voice])
        vawt_noise[voice] = 0x1103517u + (uint32_t)voice * 0x9e3779b9u;
    p1 = vawt_p1[voice];
    p2 = vawt_p2[voice];
    ps = vawt_ps[voice];
    /* Hoisted per-sample state: load once, keep in registers, store once.
     * Bit-exact: only the last hold_val/lp matters across blocks, and the
     * LFSR/hold_acc sequences are identical, just without .bss traffic. */
    uint32_t nz_state = vawt_noise[voice];
    uint32_t hold_acc = vawt_hold_acc[voice];
    int32_t hold_val = vawt_hold_val[voice];
    int32_t lp = vawt_lp[voice];
    /* Loop-invariant hoists, all bit-exact: dt* feed the same BLEP
     * residual, lim is sym-only, mix_inv is block-constant. */
    uint32_t dt1 = (uint32_t)inc1 >> 16;
    uint32_t dt2 = (uint32_t)inc2 >> 16;
    uint32_t dts = (uint32_t)incs >> 16;
    int32_t lim = 32767 - (sym < 0 ? -sym : sym);
    int32_t mix_inv = 127 - mix;

    for (i = 0; i < 32; i++) {
        int32_t s1, s2, s, nz, k;
        p1 += (uint32_t)inc1;
        p2 += (uint32_t)inc2;
        ps += (uint32_t)incs;
        nz = need_nz ? (int32_t)(lfsr_next(&nz_state) >> 16) - 32768 : 0;

        switch (wave) {
        case 0: {
            int32_t q1 = (int32_t)(p1 >> 16), q2 = (int32_t)(p2 >> 16);
            uint32_t t1 = p1 >> 16;
            s1 = q1 - 32768;
            s1 -= blep_fall(t1, dt1);
            if (use_s2) {
                uint32_t t2 = p2 >> 16;
                s2 = q2 - 32768;
                s2 -= blep_fall(t2, dt2);
            } else {
                s2 = 0;
            }
            break;
        }
        case 1:
        case 2: {
            uint32_t t1 = p1 >> 16;
            uint32_t u1 = (uint32_t)(p1 - pwth) >> 16;
            s1 = p1 < pwth ? 32767 : -32767;
            s1 += blep_fall(t1, dt1) - blep_fall(u1, dt1);
            if (use_s2) {
                uint32_t t2 = p2 >> 16;
                uint32_t u2 = (uint32_t)(p2 - pwth) >> 16;
                s2 = p2 < pwth ? 32767 : -32767;
                s2 += blep_fall(t2, dt2) - blep_fall(u2, dt2);
            } else {
                s2 = 0;
            }
            break;
        }
        case 3: {
            int32_t q1 = (int32_t)(p1 >> 16);
            s1 = q1 < 32768 ? q1 * 2 - 32768 : 98302 - q1 * 2;
            if (use_s2) {
                int32_t q2 = (int32_t)(p2 >> 16);
                s2 = q2 < 32768 ? q2 * 2 - 32768 : 98302 - q2 * 2;
            } else {
                s2 = 0;
            }
            break;
        }
        case 4:
            s1 = dnm_sin(p1);
            s2 = use_s2 ? dnm_sin(p2) : 0;
            break;
        case 5:
            s1 = wt_sample_fr(wa0, wa1, p1, ff4, use_ff);
            s2 = use_s2 ? wt_sample_fr(wa0, wa1, p2, ff4, use_ff) : 0;
            break;
        case 6:
            s1 = nz;
            s2 = nz;
            break;
        default:
            /* Blank slot: silence. */
            s1 = 0;
            s2 = 0;
            break;
        }

        if (use_s2)
            s = (int32_t)(((s1 * mix_inv + s2 * mix) >> 7) * 205 >> 8);
        else
            s = (int32_t)(((s1 * 127) >> 7) * 205 >> 8);
        if (use_sub) {
            /* Rounded square: mostly square so it cuts through, with the
             * triangle mixed in to soften the edges. Same peak as the old
             * sine at full (so SUB keeps its 0.1a level). The square part
             * gets the same PolyBLEP treatment (rising at wrap, falling
             * at half); the triangle is benign uncorrected. */
            int32_t q = (int32_t)(ps >> 16);
            int32_t tri = q < 32768 ? q * 2 - 32768 : 98302 - q * 2;
            int32_t sq = ps < 0x80000000u ? 32767 : -32767;
            uint32_t ts = ps >> 16;
            uint32_t us = (uint32_t)(ps - 0x80000000u) >> 16;
            sq += blep_fall(ts, dts) - blep_fall(us, dts);
            s += ((sq * 3 + tri) * sublvl) >> 10;
        }
        if (noilvl)
            s += (nz * noilvl) >> 9;

        if (fold)
            s = (s * gain) >> 7;    /* fold 0 is s*128>>7 = s, exact */
        {
            /* Asymmetric fold around the symmetry offset: the rail
             * tightens by |sym| so the recentered output still peaks at
             * DNM_PEAK. sym == 0 is the exact old loop. */
            s += sym;
            for (k = 0; k < 8 && (s > lim || s < -lim); k++)
                s = s > 0 ? (lim << 1) - s : (-lim << 1) - s;
            s -= sym;
        }
        if (do_crush) {
            s &= ~cmask;
            hold_acc += step;     /* sample-rate reduction */
            if (hold_acc >= (1u << 16)) {
                hold_acc -= 1u << 16;
                hold_val = s;
            }
            s = hold_val;
        } else {
            hold_val = s;         /* OFF is identity, but keep
                                   * the hold value fresh so a
                                   * later switch back behaves
                                   * exactly as before */
        }
        if (do_lp) {
            int32_t y = lp + (int32_t)(((s - lp) * lpc) >> 8);
            lp = y;
            s = y;
        } else {
            lp = s;
        }
        block[i] = s << 12;
    }
    vawt_p1[voice] = p1;
    vawt_p2[voice] = p2;
    vawt_ps[voice] = ps;
    vawt_noise[voice] = nz_state;
    vawt_hold_acc[voice] = hold_acc;
    vawt_hold_val[voice] = hold_val;
    vawt_lp[voice] = lp;
}

/* MACHINES-menu tile: the author's 16x16 grid, verbatim (top row
 * first, bit 15 the left pixel). */
static const uint16_t vawt_icon[16] = {
    0x0000, 0x0030, 0x0030, 0x00CC, 0x00CC, 0x30CC, 0x30CC, 0xCCC3,
    0xCCC3, 0x0CC0, 0x0CC0, 0x0CC0, 0x0CC0, 0x0300, 0x0300, 0x0000,
};

static const int16_t vawt_defaults[DNM_KNOBS] = {
    0, 2304, 0, 512, 16128, 0, 0, 0,
    16384, 0, 0, 32512, 32512, 16384, 0, -1,
};

const struct dnm_machine vawt_machine = {
    10, "VAWT", vawt_icon, vawt_knobs, vawt_render, 0, vawt_defaults,
};
