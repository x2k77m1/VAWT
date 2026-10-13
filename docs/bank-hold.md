# BANK switching wedges hold-to-menu (upstream core bug, not VAWT)

Symptom (on hardware and in digiemu): after BANK → trig 16 → trig 1 to an
empty pattern, holding a track key no longer opens the Mod Menu, so VAWT
cannot be picked. Reproduced in digiemu against the settled snapshot
(`Session` + `run_script`, `devices/` overlay from a fwcheck folder).

## Findings (emulator RAM + screens)

- Core tracks pressed keys in `keys_down` (core-dn1 `menu.s`, here at
  `0x47BE0FB4`). A hold only arms when no other key is down.
- The bank popup eats the trig keys' *releases*: their presses set bits in
  `keys_down`, the releases never reach `core_dn_key`. After BANK/16/1 the
  bitmap reads `0000000000000204` — trig 9 and trig 16 stuck forever.
- Every later hold fails the "went down alone" test, so the Mod Menu never
  opens. Stock is unaffected (it doesn't use this bitmap).
- Separately, on the empty pattern a single track press opens the stock
  sound browser ("SOUND 1") — reproduced on the stock build too, so that
  half is Elektron's own behaviour for a soundless track, not a mod bug.

## Workarounds (until core is fixed)

- Re-tap the bank trigs on a plain screen (e.g. tap 16, tap 9): their
  releases reach core and clear the bits. Verified: bitmap back to zero
  and the next hold opens MACHINES.
- Or reboot (fresh `.bss`).

## Proper fix (upstream, core-dn1)

`keys_down` must survive releases that a modal consumes — e.g. reconcile
against the firmware's own key state, or re-derive rather than latch.
A machine mod cannot fix this: core owns all four UI key sites. Report
with the repro above to the elekloader maintainer.

### Fix implemented and proven (against core 3.2; numbering to maintainer)

`others_down` (`mods/core-dn1/menu.s`) now only counts FUNC, MIDI and the
track keys as hold-blocking chords. Trig/page/etc. keys can't wedge it:
their modal-eaten releases were the stale bits. Double presses still arm,
FUNC+track mute and MIDI-track select still suppress the menu. One file,
29+/12-. No version bump in the change — releases are numbered upstream.

Proof (digiemu, fixed core + machines + VAWT, `fwcheck` PASS, margin 31.7%):

- Same stale bitmap as before (`keys_down = …0204`, trigs 9+16 stuck) —
  yet the next hold opens MACHINES (`menu_open = 01`, tile grid on screen).
- Post-bank-flow submenu fully navigable (FM/VAWT tiles, VAWT highlighted).
- `tests/test_units.py`: 17 passed. `tests/test_link.py`'s one failure
  reproduces identically on pristine 3.2 here (Digitakt-oriented test fed
  Digitone files — environmental, unrelated).

Upstream PR: `irpina/elekloader#73` (`Count only chord keys for
hold-to-menu`, draft), from branch `core-dn1-hold-chords` — the one file
plus this evidence.
