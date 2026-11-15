# Debug chapter start and the stage sweep

A debug-only way to start the game in any story chapter, and a script that loads every chapter in turn and records
what breaks: crashes, hangs, "no recompiled version" stops, ERROR lines, shader failures and black screens.

## Starting in a chapter: `--chapter`

```
mua3.exe --chapter 3 [--save-template <folder>]
mua3.exe --stage ST030
```

Both need `[debug] stage_select=1` in `mua3.ini` or `MUA3_DEBUG_STAGE=1` in the environment. Without that, the
arguments are ignored (with a log line) and the game starts normally.

How it works (`src/app/debug_chapter.cpp`): the game's CONTINUE loads the chapter recorded in the save. That means
`SCENARIO/ChapterSCDATA/CHAPTER_<N>`, then that chapter's `MISSION_<N>_xx` scenario files, then its first stage. In this
mode the game plays from its own save folder, `debug_save` in the working directory, instead of the configured one, so
the user's save is never opened. The chapter fields of that save are set before the game starts. Choosing CONTINUE on
the title screen then starts chapter N at its beginning, with the save's party.

- **The save fields** (the file is plain, with no checksum):
  - `save00` holds two copies of the game data, 0x5be00 bytes apart. In each copy, the u16 at +0x4d412 and the u16 at
    +0x4d43a hold the chapter.
  - `savedatainfo` holds the title screen's summary; its u16 at +0x16 is the chapter.
- **Where the save comes from:** `debug_save` needs a save to start from. `--save-template <folder>` copies `save00`
  and `savedatainfo` from that folder when `debug_save` is empty. Without a template, start a NEW GAME once in this
  mode; it autosaves in ST010, and `--chapter` works from then on.
- **What it covers:** a chapter starts at its beginning. Checkpoints inside a chapter are kept in memory, not in the
  save, so this mode can't start at a later mission of a chapter.
- **`--stage`:** takes `ST010` to `ST120`, the first stages of chapters 1 to 12. Other stages (for example ST081,
  ST091) are loaded later inside their chapter.
- **mua3.ini:** the ini keeps the user's own `save_dir` when the game rewrites it.

The test instances use it through `inst.ps1 tochapter <i> <N>`. It boots with `--chapter N`, copies a fresh
`debug_save` from `build\testinst\baseline_save` (a Chapter 1 save from `togame`), chooses CONTINUE and waits for
gameplay.

## The sweep: `build\testinst\sweep_stages.ps1`

```
powershell -File build\testinst\sweep_stages.ps1 [-Only 3,5,10] [-Redo] [-Max 5] [-Seconds 60] [-Inst 1]
```

For each chapter, the sweep:

1. boots the test instance with `inst.ps1 tochapter`, hidden and muted, on CPUs 8-15 at below-normal priority. If a
   movie or event plays first, it skips it.
2. plays for about `-Seconds` seconds: walks in four directions, uses light and heavy attacks and jump, and opens and
   closes the pause menu.
3. takes screenshots at the start, mid and end, plus one of the pause menu.
4. records the results.

`build\testinst\sweep\results.csv` gets one row per chapter. Its columns:

| Column | Meaning |
|---|---|
| `stage` | The stage loaded |
| `result` | One of `ok`, `errors`, `black`, `no_gameplay`, `no_stage`, `hang`, `crash` or `uncovered` (a "no recompiled version" stop) |
| `fps_median` | The median of the game's "Presenting" fps lines |
| `errors`, `uncovered`, `shader_fail` | Counts of the matching log lines |
| `black` | 1 if a screenshot is nearly black (mean brightness under 8 of 255) |
| `crash`, `hang` | 1 if the game crashed, or showed no frames for 30 s |
| `notes` | Free-text notes |

`build\testinst\sweep\chNNN\` holds that chapter's `err.log`, `run.log`, `errors.txt`, its screenshots, and any crash
files.

- **Resuming:** chapters already in `results.csv` are skipped unless `-Redo` is given. `-Only` picks chapters.
- **Rescoring:** `-Rescore` recomputes every row from the saved chapter folders, for after the checks change.
- **Contact sheet:** `build\testinst\sweep_sheet.ps1 [-Only ...]` writes one image with each chapter's
  screenshots in a row.
- **Chapter 0:** CONTINUE does not start chapter 0, so it ends as `no_stage`.
- **Timing:** a chapter takes about 2-5 minutes (boot, load, 60 s of play), so all 313 chapters take most of a day.
  Story chapters 1-13 come first in the list.
- **The chapter list:** the sweep writes `build\testinst\sweep\chapters.txt` from the installed RomFS
  (`SCENARIO\ChapterSCDATA`) on its first run. It is derived from game data, so it stays local and is never committed,
  as are the results.
- **Safety:** the sweep holds the test lock and checks its token before every action. It never touches `GameFiles` or
  the user's save. It does not start, and it stops, when D: has less than 10 GB free.

## Finding where the game loads a file: `MUA3_TRACE_OPEN`

`MUA3_TRACE_OPEN=<text>` logs the guest registers and call stack for the first read of each RomFS file whose path
contains `<text>`. Most game files are read by the game's file loader thread, so for those the stack ends in the
loader, not in the code that asked for the file. The game names its files by index into a path table, the relocated
pointers at main+0x20a7848 in 16-byte entries; for example, `CHAPTER_000` is file 0x662.
