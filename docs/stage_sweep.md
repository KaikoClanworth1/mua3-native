# Debug chapter and checkpoint start, and the stage sweep

A debug-only way to start the game at any checkpoint of any story chapter, and a script that loads every checkpoint
in turn and records what breaks. It looks mainly for shader problems: failed translations or compiles, unimplemented
features, variants made at runtime, shader hitches, and programs the prewarm didn't know. It also records crashes,
hangs, "no recompiled version" stops, ERROR lines and black screens.

## Starting at a checkpoint: `--chapter` and `--checkpoint`

```
mua3.exe --chapter 3 [--checkpoint 1:0] [--save-template <folder>]
mua3.exe --stage ST030
```

These need `[debug] stage_select=1` in `mua3.ini` or `MUA3_DEBUG_STAGE=1` in the environment. Without that, the
arguments are ignored (with a log line) and the game starts normally.

How it works (`src/app/debug_chapter.cpp`): the game's CONTINUE resumes the save's chapter at the save's checkpoint.

1. It loads `SCENARIO/ChapterSCDATA/CHAPTER_<N>` and the chapter's `MISSION_<N>_xx` scenario files.
2. It restores the party at the checkpoint and loads the stage areas around it.

In this mode the game plays from its own save folder, `debug_save` in the working directory, instead of the configured
one, so the user's save is never opened. The chapter and checkpoint fields of that save are set before the game starts.
Choosing CONTINUE on the title screen then starts there, with the save's party. The restore is the game's own: the
debug start only sets where it resumes.

- **Checkpoints:** a checkpoint is `section:point`.
  - The section is the chapter's mission: chapter 3's sections 0-3 are `MISSION_003_00` to `_03`. The point is a
    position within that section.
  - `DATA/DataSavePoint.bin` lists them. It has 164 records, covering the story chapters, of two kinds: S.H.I.E.L.D.
    save points and the autosave points between them.
  - Each record is 44 bytes, after a u32 count at +4. The fields are:

    | Offset | Field |
    |---|---|
    | u32 +4 | chapter |
    | u32 +8 | section |
    | u32 +0x10 | point |
    | u8 +0x19 | 1 for a S.H.I.E.L.D. point |

  - The log line says which record the checkpoint matches, or that it isn't listed.
  - Example: chapter 3, checkpoint 1:0 starts on the S.H.I.E.L.D. point inside the Shadowland dojo, not at the
    chapter start.
- **The save fields:** the file is plain, with no checksum.
  - `save00` holds two copies of the game data, 0x5be00 bytes apart. In each copy:
    - the u16s at +0x4d412 and +0x4d43a hold the chapter;
    - the u8 at +0x4d43c holds the section, and the u8 at +0x4d43d the point.
  - `savedatainfo` holds the title screen's summary; its u16 at +0x16 is the chapter.
- **Where the save comes from:** `debug_save` needs a save to start from. `--save-template <folder>` copies `save00`
  and `savedatainfo` from that folder when `debug_save` is empty. Without a template, start a NEW GAME once in this
  mode; it autosaves in ST010, and `--chapter` works from then on.
- **`--stage`:** takes `ST010` to `ST120`, the first stages of chapters 1 to 12. Other stages (for example ST081,
  ST091) are loaded later inside their chapter.
- **mua3.ini:** the ini keeps the user's own `save_dir` when the game rewrites it.

The test instances use it through `inst.ps1 tochapter <i> <N> [S:P]`. It boots with `--chapter N --checkpoint S:P`,
copies a fresh `debug_save` from `build\testinst\baseline_save` (a Chapter 1 save from `togame`), chooses CONTINUE
and waits for gameplay.

## The sweep: `build\testinst\sweep_stages.ps1`

```
powershell -File build\testinst\sweep_stages.ps1 [-Chapters 1,3] [-Starts] [-Redo] [-Rescore] [-Max 5] [-Seconds 45]
```

`-Chapters` limits the sweep to those chapters, and `-Starts` to chapter starts (0:0) only. When launching it through
`Start-Process`, pass the script through `-Command "& '<script>' -Chapters 1,3"`. With `-File`, "1,3" arrives as one
string.

For each checkpoint, the sweep:

1. boots the test instance with `inst.ps1 tochapter`, hidden and muted, on CPUs 8-15 at below-normal priority. If a
   movie or event plays first, it skips it.
2. plays for `-Seconds` seconds: walks, uses attacks and jump, turns the camera both ways, and opens and closes the
   pause menu.
3. takes screenshots at the start, mid and end, plus one of the pause menu.
4. stops the game and records the results.

`build\testinst\sweep\results.csv` gets one row per checkpoint, keyed `chapter:section:point`:

| Column | Meaning |
|---|---|
| `kind` | `saveN` (a S.H.I.E.L.D. point), `auto` (an autosave point) or `start` (a chapter with no records) |
| `stage` | The stage loaded |
| `result` | The first that applies: `uncovered`, `crash`, `hang`, `no_stage`, `no_gameplay`, `black`, `shader`, `errors`, `ok` |
| `fps_median` | The median of the game's "Presenting" fps lines |
| `shader_failed` | `[ERROR] [Shader]` lines and "failed in an earlier run" warnings |
| `unimplemented` | Unimplemented shader features in programs translated after the chapter loaded (the prewarm translates every program at startup; those lines are not counted) |
| `runtime_variants` | "variant X of Y" lines after the chapter loaded |
| `shader_hitches`, `hitch_max_ms`, `hitch_driver_shaders` | `[Hitch]` frames with shader work in them (a compile wait, first-seen programs or variants, driver shader objects created), the longest, and their driver shaders. Loading hitches (texture uploads only) are left out |
| `new_programs`, `new_variants` | Lines this checkpoint added to the instance's `shader_dump\order.txt` / `variants.txt`: shaders the prewarm did not know |
| `errors`, `uncovered` | Counts of the matching log lines |
| `black` | 1 if every gameplay screenshot is nearly black (mean brightness under 4 of 255; dark areas such as the Raft's cells measure about 6 with the HUD) |
| `crash`, `hang` | 1 if the game crashed, or showed no frames for 30 s |

Other output:

- `build\testinst\sweep\cNNN_S_P\` holds the checkpoint's `err.log`, `run.log`, `errors.txt`, `shaders.txt` (the
  shader lines behind the counts), `new_order.txt` / `new_variants.txt`, its screenshots and any crash files.
- **Prewarm lists:** `build\testinst\sweep\new_order.txt` and `new_variants.txt` collect every new program and variant
  in the game's own `shader_dump` line formats, deduplicated. `new_by_checkpoint.csv` says where each was found.

Usage notes:

- **Resuming:** checkpoints already in `results.csv` are skipped unless `-Redo` is given.
- **Rescoring:** `-Rescore` recomputes every row from the saved folders, for after the checks change.
- **Contact sheet:** `build\testinst\sweep_sheet.ps1 [-Chapters ...]` writes one image with a row of screenshots
  per checkpoint.
- **The checkpoint list:** the sweep writes `build\testinst\sweep\checkpoints.txt` from the installed RomFS on its
  first run (464 entries). It is derived from game data, so it stays local and is never committed, as are the results
  and the lists.
- **Timing:** a checkpoint takes about 2-4 minutes, so the whole list takes about a day. The 164 story checkpoints
  alone take about 8 hours.
- **Chapter 0:** CONTINUE does not start chapter 0, so it ends as `no_stage`.
- **Safety:** the sweep holds the test lock and checks its token before every action. It never touches `GameFiles` or
  the user's save. It does not start, and it stops, when D: has less than 10 GB free.

## Finding where the game loads a file: `MUA3_TRACE_OPEN`

`MUA3_TRACE_OPEN=<text>` logs the guest registers and call stack for the first read of each RomFS file whose path
contains `<text>`. Most game files are read by the game's file loader thread, so for those the stack ends in the
loader, not in the code that asked for the file. The game names its files by index into a path table, the relocated
pointers at main+0x20a7848 in 16-byte entries; for example, `CHAPTER_000` is file 0x662.
