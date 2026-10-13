# Custom wavetables: build your own VAWT .elemod

The factory build ships 24 banks (24 bundled tables, all replaceable).
This page builds a personal `dnvawt-*.elemod` from a folder of your own
single-cycle `.wav` files, for use in elekloader exactly like the
factory one. No firmware knowledge needed: one command, checked at
every step.

## What you need

- Python 3, `numpy` (`pip install numpy`), the m68k toolchain
  (`m68k-elf-gcc` reachable, or set `ELEKLOADER_CROSS`), this repo with
  an elekloader checkout at `tools/elekloader` (separate project, gitignored).
- Elektron's stock OS `.syx` for your unit — the same file you will patch
  from. It selects the OS-version ports; never flash from here.
- A flat folder of `.wav` files, one wavetable each: mono 16-bit WAVs
  holding equal single-cycle frames, named
  `<anything> <n>_<framelen>_<frames>_<rate>_16bit.wav` (`n` = table
  number for `--pick`). Frame count, length and rate may differ per
  file. Anything else in the folder is skipped with a warning.

## One command

```sh
python3 tools/build-tables.py --wavs DIR --stock STOCK.syx --out custom/
```

This validates the folder, converts up to 22 tables (8 evenly spread
frames each, FFT-resynthesized to 256 samples, harmonics capped at 120,
peak-normalized), builds the `.elemod`, writes a manifest beside it
(bank map, stock checksum, timestamp), and runs the full host DSP test
suite against *your* tables — a failure aborts the build.

Useful flags: `--pick 1-11,25` (subset by table number),
`--tag X` (labels manifest entries `X01…`; display stays `WT1…WTN`;
default from the folder name),
`--with-builtins` (prepend the two additive tables as WT1-2 as ordinary entries),
`--version VER` (stamp; default follows the base mod),
`--patch` (also build core+machines for the stock file, lint, and write
a ready-to-flash custom `.syx`), `--keep-temp` (debug the temp mod dir).

## Rules and limits

- **Exactly one dnvawt per build** (factory *or* custom, never both):
  both claim the same mod id, sound slot, parameters and page sites, so
  elekloader refuses the pair. Everything else combines identically —
  custom tables are internal data, no new resource claims.
- **24 banks resident, all replaceable**: mod RAM is 128KB shared.
  Over-picks fail fast with the count. `--with-builtins` prepends the
  two additive tables as WT1-2 as ordinary entries. Want another set? rebuild with
  another `--pick` — indices are append-only, so kept banks never move.
- Display names stay `WT1…WTN`; the manifest + build log map each to its
  source file. TABL knob, WPOS morph, p-locks, presets and MIDI CC work
  as in the factory build.

## Troubleshooting

| problem | fix |
|---|---|
| `numpy is missing` | `pip install numpy` |
| `m68k-elf-gcc not found` | install the toolchain / set `ELEKLOADER_CROSS` |
| `--stock … not found` | point at Elektron's stock `.syx` |
| `no wavetables in …` | folder must directly hold the `.wavs` |
| `ceiling is 22` | narrow `--pick`; another set = another build |
| `asks for missing tables […]` | numbers must exist in the folder |
| host tests fail | the tables are at fault (DC offset/clipping are
  normalized away; anything else is exotic) — inspect the file |
| lint/patch fails | same gates as factory builds; file an issue with
  the manifest + log |
