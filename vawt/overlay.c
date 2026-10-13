/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VAWT's pages: full SYN1/SYN2 redraws on a machine track
 * (digitone-mk1/machines.h, core-dn1 3.1).
 *
 * Stock SYN pages draw FM's pictures (the algorithm diagram, the envelope
 * graphs) and format values as FM, which core 3.1 cannot replace per track.
 * This paints the whole content area on ev_draw, after the firmware drew the
 * frame: an 8-knob grid with our labels, our true value texts and mini
 * graphics. Title bar, left rail and status bar stay stock.
 *
 * Gating uses firmware state, not pixels: paint only when the active
 * track's sound plays VAWT, the showing key is SYN1/SYN2 (one firmware
 * address), the brain shows no screen over the pages, and the shown view
 * is on its first page. Pixel probes are gone on purpose: header text
 * (page title vs pattern name) and value-dependent chrome made them pass
 * for one title or value and fail for another, so the overlay showed for
 * a few seconds and vanished, and matched SYN second pages too. A menu,
 * browser or any overlay screen fails the brain check; a second page
 * fails the first-page check; anything else fails the key check. A held
 * sequencer step does NOT fail: in GRID or STEP record the overlay stays
 * up and shows that step's lock values instead (rows are slot-indexed:
 * the step row's word for the knob's sound slot, -1 = none, count at
 * word 79); otherwise the trigs play notes and the kit shows, as stock.
 * A
 * copy/paste/clear toast does NOT fail the brain check either (same view),
 * so RECORD/STOP/PLAY with FUNC, PTN, PAGE or a held trig yields the
 * frame for ~90 ticks while the firmware's message shows. The
 * core menu paints over every ev_draw handler anyway. Values come from
 * the kit (what the knobs show) — or, while a sequencer step is held,
 * from that step's parameter locks (read from the pattern's lock table,
 * inverted where locked, as the manual's p-lock pages do); the last-turned
 * encoder's cell gets a frame. ev_tick re-arms the frame only when something the overlay shows changed
 * (page, values, highlight), so the overlay is there from the first frame
 * and tracks the kit without redrawing 30 times a second and starving the
 * UI task. Runs on the UI task.
 */
#include "digitone-mk1/machines.h"
#include "wt_tables.h"

#define VAWT_ID     10

/* UI-tick counter, shared with vawt.c's polyphony fuse (last-seen stamps
 * and vawt_reap's horizon). Single aligned word: the interrupt-level
 * render only ever reads it, so no lock needed. */
uint32_t vawt_ticks;
void vawt_reap(void);

extern const struct dnm_knob vawt_knobs[DNM_KNOBS];

#ifndef KEYVAR_AT
#error "KEYVAR_AT: the showing key's address (0x41367ce4 in 1.43)"
#endif
#ifndef PATPTR_AT
#error "PATPTR_AT: the active-pattern pointer (0x4138e214 in 1.43)"
#endif
#ifndef PROJPTR_AT
#error "PROJPTR_AT: the Project object pointer (0x419efde0 in 1.43)"
#endif

/* Brain/screen chain, as core_page_shown uses it (core-dn1 menu.s): the
 * key dispatcher's brain keeps the screens shown at +0x54 (bottom) and
 * +0x5c (top); the bottom screen's page manager is at +8 and its shown
 * view at +0xc0; a view's place in its list is at +144. */
#define BRAIN_BOTTOM    0x54
#define BRAIN_TOP       0x5c
#define SCREEN_MGR      8
#define MGR_VIEW        0xc0
#define VIEW_CUR        144

/* Content area and grid (panel coords, y = 0 top). The clear rect starts
 * 3 px left of the grid (CXL < CX0): the stock algorithm diagram animates
 * with the WAVE value and its squares reach x16-17 on ALGO 7/8, while the
 * rail provably ends at x14 (measured vs stock) — so clearing from 15
 * takes the bleed without touching rail chrome. Cells stay at CX0. */
#define CX0 18
#define CXC 15
#define CX1 127
#define CY0 12
#define CY1 63
#define COLS 4
#define ROWS 2

static uint32_t vawt_frames;
static uint32_t vawt_enc_at;
static int32_t vawt_enc = -1;
static int32_t vawt_enc_page = -1;
static void *vawt_brain;
/* Held sequencer-step keys, one bit a trig (key ids 26-41). The record
 * mode decides what a hold means (manual 10.2/10.9.1): GRID or STEP shows
 * that step's lock values (inverted where locked); OFF or LIVE plays notes
 * and the kit shows, as stock. Transitions (panel section, 10.2.5/10.2.6):
 * RECORD alone toggles grid (and takes GRID from LIVE, exits STEP);
 * RECORD+PLAY enters LIVE; RECORD+STOP enters STEP; PLAY or STOP alone
 * exit LIVE; STOP/PLAY otherwise leave GRID/STEP alone. The mode is
 * tracked here rather than read from firmware RAM: these chords are the
 * only paths in and out, and the Mod Menu consumes keys before stock and
 * mods alike see them, so a press the menu eats moves neither side and the
 * bits cannot desync. Page keys TRIG..LFO are ids 20-25. */
static uint32_t vawt_trigs;
static uint32_t vawt_edit_rec;  /* GRID or STEP: holds address steps */
static uint32_t vawt_live;      /* LIVE RECORDING: holds play notes */
static uint32_t vawt_rec_down;
/* Copy/paste/clear message yield: FUNC=1, PTN=4, PAGE=19 held state, and a
 * tick deadline while the overlay stays off so the firmware's transient
 * "COPY/PATTERN ..." message shows. Copy/paste/clear all press RECORD=10,
 * PLAY=11 or STOP=12 with a modifier (FUNC, PTN, PAGE) or a held trig; the
 * message is drawn in the same view (no new brain screen), so the brain
 * check cannot catch it and the overlay would otherwise paint over it. */
static uint32_t vawt_func_down;
static uint32_t vawt_ptn_down;
static uint32_t vawt_page_down;
static uint32_t vawt_msg_until;
#define VAWT_MSG_HOLD 90u

static int32_t msg_suppressed(void)
{
    return (int32_t)(vawt_ticks - vawt_msg_until) < 0;
}

/* y_top -> Bitmap y (0 at the bottom). */
#define YB(yt) (63 - (yt))

static void cell_bar(void *bmp, int32_t x, int32_t ytop, int32_t w,
                     int32_t val, int32_t max, int32_t center)
{
    int32_t yb = YB(ytop), fill, f0, f1;
    if (val < 0)
        val = 0;
    if (val > max)
        val = max;
    fw_framerect(bmp, x, yb - 4, x + w, yb, 1);
    if (!center) {
        fill = (w * val) / max;
        if (fill > 0)
            fw_fillrect(bmp, x + 1, yb - 3, x + fill, yb - 1, 1);
        return;
    }
    fill = (w * val) / max;               /* bipolar: from the middle */
    f0 = x + 1 + w / 2;
    f1 = x + 1 + w / 2 + fill - w / 2;
    if (f1 < f0) {
        int32_t t = f0;
        f0 = f1;
        f1 = t;
    }
    if (f1 > f0)
        fw_fillrect(bmp, f0, yb - 3, f1, yb - 1, 1);
    fw_fillrect(bmp, x + w / 2, yb - 3, x + 1 + w / 2, yb - 1, 1);
}

static void draw_cell(void *bmp, int32_t k, int32_t x, int32_t ytop,
                      int32_t w, int32_t value, int32_t sel, int32_t locked)
{
    char buf[16];
    const char *name = vawt_knobs[k].name;
    if (!name || !name[0])
        return;
    fw_textf(bmp, fw_font5, x + 1, YB(ytop) - 7, -1, "%s", name);
    vawt_knobs[k].fmt(value, buf);
    fw_textf(bmp, fw_font5, x + 1, YB(ytop) - 14, -1, "%s", buf);
    if (locked)
        fw_fillrect(bmp, x, YB(ytop) - 19, x + w, YB(ytop) - 9, -1);
    if (k == 4)
        cell_bar(bmp, x + 1, ytop + 19, w - 2, (value >> 8) - 37, 52, 1);
    else if (k == 1)
        cell_bar(bmp, x + 1, ytop + 19, w - 2, value >> 8, 18, 1);
    else if (k == 2)
        cell_bar(bmp, x + 1, ytop + 19, w - 2, value >> 8, 35, 0);
    else if (k == 5 || k == 6 || k == 7 || (k >= 8 && k <= 12))
        cell_bar(bmp, x + 1, ytop + 19, w - 2, value >> 8, 127, 0);
    else if (k == 14)
        cell_bar(bmp, x + 1, ytop + 19, w - 2, value >> 8, WT_BANKS - 1, 0);
    /* Last, and short of the mini-bar zone: the outline covers label +
     * value only (panel ytop..ytop+17; bars start at +19), so no bar
     * crosses it on any knob. */
    if (sel)
        fw_framerect(bmp, x, YB(ytop + 17), x + w, YB(ytop), 1);
}

static int knob_slot(int32_t k)
{
    /* Slot 33 is stock SYN2-G (param 125), the TABL knob: sequential
     * after the proven 119-124 -> 27-32 run (hardware-verified by
     * TABL-turn isolation: only the table may move). Index 15 (P) is
     * unused. */
    static const uint8_t slots[15] = {
        17, 18, 19, 20, 21, 22, 23, 24, 27, 28, 29, 30, 31, 32, 33,
    };
    return k < 0 || k > 14 ? -1 : slots[k];
}

/* Held step for lock display: lowest held trig key (ids 26-41) as step
 * trig-1 on the shown page (page 1:4 assumed); -1 = none held. */
static int32_t held_step(void)
{
    uint32_t m = vawt_trigs, i;
    if (!m)
        return -1;
    for (i = 0; i < 16; i++)
        if (m & (1u << i))
            return (int32_t)i;
    return -1;
}

/* Lock-table addresses (1.43; 1.44 is the same RAM 0x1000 up). Hardware
 * photos (VAWT/SYN1, held empty step before/after turning DET) prove:
 * pool headers are slot+track live-first (FFFF -> 1500 for DET slot 21 on
 * track 0); the step row holds the lock at its sound-slot word (word 0x15
 * = 5900, raw 89 = DET max) with -1 = none; the step's lock count is at
 * word 79 (0000 -> 0001); the kit slot is untouched (K21 stays 3F00).
 * So rows are slot-indexed, not packed by pool column order. */
#define LOCK_POOL_COLS  80
#define LOCK_POOL_STEP  130u
#define LOCK_POOL_OFF   0x1e84u
#define LOCK_TAB_OFF    0x1e80u
#define LOCK_TRACK_STR  10319u
#define LOCK_STEP_STR   160u
#define LOCK_PAT_SIZE   0x4800u
#define LOCK_PAT_OFF    0x200u
#define LOCK_PAT_NO     0x16119

/* One lock value from the RAM row by sound slot, -1 = none. OCT lives on
 * slot 20 like the rest (its param 182 only matters on the page), so the
 * slot index covers it too. */
static int32_t lock_read(void *pattern, int32_t track, int32_t step,
                         int32_t slot)
{
    uint8_t *row;
    int16_t v;
    if (!pattern || step < 0 || step > 63 || slot < 0 || slot > 78)
        return -1;
    row = (uint8_t *)pattern + LOCK_TAB_OFF + (uint32_t)track * LOCK_TRACK_STR +
        (uint32_t)step * LOCK_STEP_STR;
    v = *(volatile int16_t *)(row + (uint32_t)slot * 2u);
    return (int32_t)v;
}

/* What the overlay shows for the page's eight knobs: the held step's lock
 * values where locked (bit i of the mask), else the kit. */
static void shown_state(uint8_t *kit, int32_t track, int32_t page,
                        int32_t *vals, int32_t *lockedmask)
{
    int32_t step = held_step();
    int32_t locked = 0;
    void *pattern = 0;
    int32_t i;
    if (step >= 0 && vawt_edit_rec)
        pattern = *(void *volatile *)PATPTR_AT;
    for (i = 0; i < 8; i++) {
        int32_t k = (page - 1) * 8 + i, slot, v = 0x7fffffff;
        if (k > 14) {
            vals[i] = v;
            continue;
        }
        slot = knob_slot(k);
        v = slot < 0 ? 0x7fffffff : FW_SOUND_SLOT(kit, track, slot);
        if (step >= 0 && pattern && slot >= 0) {
            int32_t lv = lock_read(pattern, track, step, slot);
            if (lv != -1) {
                v = lv;
                locked |= 1 << i;
            }
        }
        vals[i] = v;
    }
    *lockedmask = locked;
}

#ifdef VAWT_LOCK_DEBUG
/* Latched last RECORD/PLAY/STOP press (non-release): the photo then shows
 * whether the toggle saw the press and with what flags. */
static int32_t vawt_dbg_rps_id = -1;
static int32_t vawt_dbg_rps_flags = 0;

/* The track's pool columns in pool order: header bytes. -> count. Kept for
 * the raw-state debug screen only (P0:/P4: vs R:/C). */
static int32_t lock_cols(int32_t track, int32_t *params)
{
    void *pattern, *mirror;
    uint8_t *pool;
    uint32_t proj;
    int32_t n, c, ncols = 0;
    pattern = *(void *volatile *)PATPTR_AT;
    if (!pattern)
        return 0;
    n = *(volatile int32_t *)((uint8_t *)pattern + LOCK_PAT_NO);
    if (n < 0 || n > 127)
        return 0;
    proj = *(volatile uint32_t *)PROJPTR_AT;
    if (!proj)
        return 0;
    mirror = *(void *volatile *)((uint8_t *)proj + 0x20);
    if (!mirror)
        return 0;
    pool = (uint8_t *)mirror + LOCK_PAT_OFF + (uint32_t)n * LOCK_PAT_SIZE +
        LOCK_POOL_OFF;
    for (c = 0; c < LOCK_POOL_COLS; c++) {
        uint8_t p = *(volatile uint8_t *)(pool + (uint32_t)c * LOCK_POOL_STEP);
        uint8_t t = *(volatile uint8_t *)(pool + (uint32_t)c * LOCK_POOL_STEP + 1);
        if (p == 0xff && t == 0xff)
            break;
        if ((int32_t)t == track)
            params[ncols++] = p;
    }
    return ncols;
}
/* Temporary diagnostic: while a step is held, show raw lock-table state
 * instead of the grid (never committed with the flag on). */
static void put_hex4(char *buf, uint32_t v)
{
    static const char h[] = "0123456789ABCDEF";
    buf[0] = h[(v >> 12) & 15];
    buf[1] = h[(v >> 8) & 15];
    buf[2] = h[(v >> 4) & 15];
    buf[3] = h[v & 15];
    buf[4] = 0;
}

static void debug_locks(void *bmp, uint8_t *kit, int32_t track)
{
    int32_t step = held_step(), cols[LOCK_POOL_COLS], ncols = 0, i;
    void *pattern = *(void *volatile *)PATPTR_AT;
    uint32_t proj = *(volatile uint32_t *)PROJPTR_AT;
    void *mirror = 0;
    int32_t n = -99;
    char b1[64], b2[64];
    if (pattern)
        n = *(volatile int32_t *)((uint8_t *)pattern + LOCK_PAT_NO);
    if (proj)
        mirror = *(void *volatile *)((uint8_t *)proj + 0x20);
    if (pattern)
        ncols = lock_cols(track, cols);
    fw_fillrect(bmp, CXC, YB(CY1), CX1, YB(CY0), 0);
    fw_textf(bmp, fw_font5, CX0 + 1, YB(CY0 + 6) - 7, -1, "T%d S%d N%d C%d",
             track, step, n, ncols);
    b1[0] = 0;
    for (i = 0; i < 8; i++) {
        /* Raw pool headers (param+track bytes) straight from the mirror,
         * not my walk: holes and order visible as-is. Two rows of four. */
        char h[8];
        int32_t k, r = i / 4;
        uint8_t *pool = 0;
        if (mirror && n >= 0 && n <= 127)
            pool = (uint8_t *)mirror + LOCK_PAT_OFF + (uint32_t)n * LOCK_PAT_SIZE +
                LOCK_POOL_OFF;
        if (pool) {
            uint8_t p = *(volatile uint8_t *)(pool + (uint32_t)i * LOCK_POOL_STEP);
            uint8_t t = *(volatile uint8_t *)(pool + (uint32_t)i * LOCK_POOL_STEP + 1);
            put_hex4(h, ((uint32_t)p << 8) | t);
        } else {
            put_hex4(h, 0xEEEEu);
        }
        for (k = 0; h[k]; k++)
            b1[r * 20 + (i % 4) * 5 + k] = h[k];
        b1[r * 20 + (i % 4) * 5 + 4] = ' ';
        if (i % 4 == 3 || i == 7) {
            b1[r * 20 + (i % 4) * 5 + 5] = 0;
            fw_textf(bmp, fw_font5, CX0 + 1, YB(CY0 + 13 + 7 * r) - 7, -1,
                     "P%d:%s", r * 4, b1 + r * 20);
        }
    }
    if (pattern && step >= 0) {
        /* Every non-empty word of the held step's row: wherever a fresh
         * lock lands shows up here regardless of mapping guesses. */
        uint8_t *row = (uint8_t *)pattern + LOCK_TAB_OFF +
            (uint32_t)track * LOCK_TRACK_STR + (uint32_t)step * LOCK_STEP_STR;
        int32_t k, shown = 0, rr = 0;
        b2[0] = 0;
        for (i = 0; i < 80 && shown < 4; i++) {
            char h[8], w[8];
            int16_t v = *(volatile int16_t *)(row + (uint32_t)i * 2u);
            int32_t j;
            if (v == -1)
                continue;
            put_hex4(w, (uint32_t)i);
            put_hex4(h, (uint32_t)(uint16_t)v);
            for (k = 0; w[k]; k++)
                b2[rr * 20 + (shown % 2) * 10 + k] = w[k];
            b2[rr * 20 + (shown % 2) * 10 + 4] = '=';
            for (j = 0; h[j]; j++)
                b2[rr * 20 + (shown % 2) * 10 + 5 + j] = h[j];
            b2[rr * 20 + (shown % 2) * 10 + 9] = ' ';
            shown++;
            if (shown % 2 == 0) {
                b2[rr * 20 + 20] = 0;
                fw_textf(bmp, fw_font5, CX0 + 1, YB(CY0 + 27 + 7 * rr) - 7, -1,
                         "R:%s", b2 + rr * 20);
                rr++;
            }
        }
        if (shown % 2) {
            b2[rr * 20 + 10] = 0;
            fw_textf(bmp, fw_font5, CX0 + 1, YB(CY0 + 27 + 7 * rr) - 7, -1,
                     "R:%s", b2 + rr * 20);
        }
        if (!shown)
            fw_textf(bmp, fw_font5, CX0 + 1, YB(CY0 + 27) - 7, -1, "R:(empty)");
        {
            /* Recorder state in one line: edit/live bits, modifier nibble
             * (FUNC<<3|PTN<<2|PAGE<<1|REC), last RPS press id+flags, trig
             * mask hex. Sits at +41: the screen hardware cuts off anything
             * lower (the +48/+55 lines never displayed). Replaces the K21
             * kit reference, whose mapping proof is done. */
            char t[8];
            int32_t mods = (vawt_func_down << 3) | (vawt_ptn_down << 2) |
                (vawt_page_down << 1) | vawt_rec_down;
            put_hex4(t, vawt_trigs & 0xffffu);
            fw_textf(bmp, fw_font5, CX0 + 1, YB(CY0 + 41) - 7, -1,
                     "E%dL%d X%d K%dF%d T%s", vawt_edit_rec, vawt_live,
                     mods, vawt_dbg_rps_id, vawt_dbg_rps_flags, t);
        }
    }
    (void)mirror;
}
#endif

void vawt_draw(void *bmp, void *ctrl)
{
    int32_t track = fw_active_track, mid, page = 0, pos;
    uint8_t *kit;
    void *bottom, *top, *mgr, *view;
    int32_t colw, rowh, r, c;
    int32_t key = *(volatile int32_t *)KEYVAR_AT;
    (void)ctrl;
    vawt_frames++;
    /* A modal yields the frame; the stuck-key sweep itself lives in
     * vawt_tick, which runs constantly, while draws only run on dirty
     * frames (too rare to rely on when idle). */
    if (!vawt_brain)
        return;
    bottom = *(void *volatile *)((uint8_t *)vawt_brain + BRAIN_BOTTOM);
    top = *(void *volatile *)((uint8_t *)vawt_brain + BRAIN_TOP);
    if (!bottom || bottom != top)
        return;
    if (track < 0 || track > 3)
        return;
    kit = fw_kit;
    if (!kit)
        return;
    mid = FW_SOUND_SLOT(kit, track, 0) >> 8;
    if (mid != VAWT_ID)
        return;
    /* The showing key picks the page: SYN1's grid or SYN2's. */
    if (key != 1 && key != 2)
        return;
    page = key;
    /* No screen over the pages, and the shown view on its first page.
     * Second pages, menus, browsers and any overlay fail here. */
    mgr = *(void *volatile *)((uint8_t *)bottom + SCREEN_MGR);
    if (!mgr)
        return;
    view = *(void *volatile *)((uint8_t *)mgr + MGR_VIEW);
    if (!view)
        return;
    pos = *(volatile int32_t *)((uint8_t *)view + VIEW_CUR);
    if (pos != 0)
        return;
    /* Copy/paste/clear messages are drawn in this same view (no new brain
     * screen), so yield while one is up: the firmware's toast shows. */
    if (msg_suppressed())
        return;
#ifdef VAWT_LOCK_DEBUG
    if (vawt_trigs) {
        debug_locks(bmp, kit, track);
        return;
    }
#endif
    {
        int32_t vals[8], locked = 0;
        shown_state(kit, track, page, vals, &locked);
        fw_fillrect(bmp, CXC, YB(CY1), CX1, YB(CY0), 0);
        colw = (CX1 - CX0 + 1) / COLS;
        rowh = (CY1 - CY0 + 1) / ROWS;
        for (r = 0; r < ROWS; r++) {
            for (c = 0; c < COLS; c++) {
                int32_t k = (page - 1) * 8 + r * COLS + c;
                int32_t x, yt, sel, li;
                if (k > 14)
                    continue;
                x = CX0 + c * colw;
                yt = CY0 + r * rowh;
                li = r * COLS + c;
                sel = k - (page - 1) * 8 == vawt_enc &&
                    vawt_enc_page == page && vawt_ticks - vawt_enc_at < 45;
                draw_cell(bmp, k, x, yt, colw - 2, vals[li], sel,
                          (locked >> li) & 1);
            }
        }
    }
}

int vawt_key_handler(void *brain, void *ev)
{
    /* Remember the key dispatcher's brain for the draw-time screen check.
     * Track held step keys for lock display (the step's values, inverted
     * where locked, and only in GRID/STEP record — otherwise a hold plays
     * notes). RECORD alone toggles grid (GRID from LIVE, out of STEP);
     * RECORD+PLAY enters LIVE, RECORD+STOP enters STEP, PLAY/STOP alone
     * exit LIVE; with FUNC/PTN/PAGE or a held trig RECORD is a copy
     * command and changes nothing. A page-key press clears the held-step
     * mask, and every tick sweeps all held-key bits while a modal is up
     * (see vawt_tick): a modal can eat a key's release (the bank popup
     * eats trig keys'), which would otherwise stick a bit — a stuck bit
     * blocks the RECORD toggle, so the sweep runs constantly on every
     * page, not just on VAWT draws. Copy/paste/clear toasts and the
     * live/step entry chords start the message yield so a firmware toast
     * shows instead of the grid. Never takes the event. */
    int32_t id, flags;
    if (brain)
        vawt_brain = brain;
    if (!ev)
        return 0;
    id = *(volatile int32_t *)((const uint8_t *)ev + 12);
    flags = *(volatile int32_t *)((const uint8_t *)ev + 16);
#ifdef VAWT_LOCK_DEBUG
    /* Latch the press for the debug photo (releases excluded). */
    if ((id == 10 || id == 11 || id == 12) && !(flags & 0x10)) {
        vawt_dbg_rps_id = id;
        vawt_dbg_rps_flags = flags;
    }
#endif
    if (id == 1) {
        if (flags & 0x10)
            vawt_func_down = 0;
        else
            vawt_func_down = 1;
    } else if (id == 4) {
        if (flags & 0x10)
            vawt_ptn_down = 0;
        else
            vawt_ptn_down = 1;
    } else if (id == 19) {
        if (flags & 0x10)
            vawt_page_down = 0;
        else
            vawt_page_down = 1;
    }
    if (id == 10) {
        if (flags & 0x10) {
            vawt_rec_down = 0;
        } else {
            vawt_rec_down = 1;
            /* Bare RECORD press (down edge, not hold repeat): LIVE ->
             * GRID, else GRID/STEP/off toggle. A chorded one is copy (or
             * live/step entry below). Exact ==1 on purpose: hold repeats
             * must not retoggle, e.g. after a modal clear mid-hold. */
            if (flags == 1 && !vawt_func_down && !vawt_ptn_down &&
                !vawt_page_down && !vawt_trigs) {
                if (vawt_live) {
                    vawt_live = 0;
                    vawt_edit_rec = 1;
                } else {
                    vawt_edit_rec ^= 1;
                }
            }
        }
    } else if (id == 11 && flags == 1) {
        if (vawt_rec_down) {
            vawt_live = 1;      /* RECORD+PLAY -> LIVE */
            vawt_edit_rec = 0;
        } else if (vawt_live) {
            vawt_live = 0;      /* PLAY exits LIVE */
        }
    } else if (id == 12 && flags == 1) {
        if (vawt_rec_down) {
            vawt_live = 0;      /* RECORD+STOP -> STEP */
            vawt_edit_rec = 1;
        } else if (vawt_live) {
            vawt_live = 0;      /* STOP exits LIVE */
        }
    }
    if (id >= 26 && id <= 41) {
        if (flags & 0x10)
            vawt_trigs &= ~(1u << (uint32_t)(id - 26));
        else
            vawt_trigs |= 1u << (uint32_t)(id - 26);
    } else if (id >= 20 && id <= 25 && (flags & 1)) {
        vawt_trigs = 0;
    }
    if (id >= 42 && id <= 45 && (flags & 1)) {
        /* A track press/hold means a track switch or an opening Mod Menu
         * (hold): the menu eats RECORD/trig/PTN/BANK releases without any
         * brain-screen change, so the tick sweep cannot see them — drop
         * all edge states here instead. A held trig across a track switch
         * must be re-held; record mode itself is untouched (it persists
         * across menus by design). */
        vawt_trigs = 0;
        vawt_func_down = 0;
        vawt_ptn_down = 0;
        vawt_page_down = 0;
        vawt_rec_down = 0;
    }
    if ((id == 10 || id == 11 || id == 12) && !(flags & 0x10)) {
        if (vawt_func_down || vawt_ptn_down || vawt_page_down || vawt_trigs ||
            ((id == 11 || id == 12) && vawt_rec_down))
            vawt_msg_until = vawt_ticks + VAWT_MSG_HOLD;
    }
    return 0;
}

int vawt_enc_handler(void *brain, void *ev)
{
    int32_t enc, delta, key;
    if (brain)
        vawt_brain = brain;
    if (!ev)
        return 0;
    enc = *(volatile int32_t *)((const uint8_t *)ev + 12);
    delta = *(volatile int32_t *)((const uint8_t *)ev + 16);
    (void)delta;
    if (enc < 1 || enc > 8)
        return 0;
    /* Page from the showing key itself, not from the last painted frame:
     * a turn can land while another screen is up, and the cached page
     * would then be stale. */
    key = *(volatile int32_t *)KEYVAR_AT;
    if (key != 1 && key != 2)
        return 0;
    vawt_enc = enc - 1;
    vawt_enc_page = key;
    vawt_enc_at = vawt_ticks;
    return 0;
}

/* Change signature for the tick: re-arm the frame only when something the
 * overlay shows actually changed. Forcing a recompose every tick keeps the
 * UI task redrawing 30 times a second, which starves key/encoder handling
 * on hardware (turns feel buffered, values land after release) and only
 * gets worse with more voices playing. */
static int32_t tick_valid;
static int32_t tick_track = -1;
static int32_t tick_key = -1;
static int32_t tick_step = -1;
static int32_t tick_vals[8];
static int32_t tick_locked;
static int32_t tick_enc = -1;
static int32_t tick_enc_page = -1;
static int32_t tick_hl;

void vawt_tick(void *ctrl)
{
    int32_t track = fw_active_track, mid, key, page, step, i, hl, locked = 0;
    int32_t dirty = 0;
    int32_t vals[8];
    uint8_t *kit;
    vawt_ticks++;                   /* the highlight clock runs always */
    if (!ctrl)
        return;
    /* Housekeeping that must run constantly, on every page: a modal can
     * eat a key's release (the bank popup eats trig keys'), which would
     * otherwise stick a trig/modifier bit — a stuck bit blocks the RECORD
     * toggle, wedging lock display off everywhere. Draws only run on
     * dirty frames, too rare to rely on when idle, so the sweep lives
     * here. vawt_reap ages out fuse voices the render stopped sending. */
    if (vawt_brain) {
        void *bottom = *(void *volatile *)((uint8_t *)vawt_brain + BRAIN_BOTTOM);
        void *top = *(void *volatile *)((uint8_t *)vawt_brain + BRAIN_TOP);
        if (!bottom || bottom != top) {
            vawt_trigs = 0;
            vawt_func_down = 0;
            vawt_ptn_down = 0;
            vawt_page_down = 0;
            vawt_rec_down = 0;
        }
    }
    vawt_reap();
    if (track < 0 || track > 3)
        return;
    kit = fw_kit;
    if (!kit)
        return;
    mid = FW_SOUND_SLOT(kit, track, 0) >> 8;
    if (mid != VAWT_ID) {
        tick_valid = 0;
        return;
    }
    key = *(volatile int32_t *)KEYVAR_AT;
    if (key != 1 && key != 2) {
        tick_valid = 0;
        return;
    }
    /* While a copy/paste/clear toast is up the overlay stays off: do not
     * re-arm, so the firmware's message keeps the frame. Expiry dirties
     * (tick_valid == 0) and the grid comes back on the next change. */
    if (msg_suppressed()) {
        tick_valid = 0;
        return;
    }
    /* What the overlay would paint for this key: the page's eight shown
     * values (locks for a held step, else the kit), which of them are
     * locked, and the encoder highlight state. */
    page = key;
    step = held_step();
    shown_state(kit, track, page, vals, &locked);
    hl = vawt_enc >= 0 && vawt_enc_page == page &&
        vawt_ticks - vawt_enc_at < 45;
    if (!tick_valid || track != tick_track || key != tick_key ||
        step != tick_step || locked != tick_locked ||
        vawt_enc != tick_enc || vawt_enc_page != tick_enc_page || hl != tick_hl)
        dirty = 1;
    else {
        for (i = 0; i < 8; i++) {
            if (vals[i] != tick_vals[i]) {
                dirty = 1;
                break;
            }
        }
    }
    if (!dirty)
        return;
    /* Something changed (or the first sighting): recompose once so the
     * overlay paints it. Without this the firmware only redraws on its own
     * dirty flag, so the overlay appears only after another SYN press. */
    tick_valid = 1;
    tick_track = track;
    tick_key = key;
    tick_step = step;
    tick_locked = locked;
    for (i = 0; i < 8; i++)
        tick_vals[i] = vals[i];
    tick_enc = vawt_enc;
    tick_enc_page = vawt_enc_page;
    tick_hl = hl;
    *((volatile uint8_t *)ctrl + 0x20) = 1;
}
