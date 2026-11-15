# Daytona USA Deluxe — technical overview

This document describes how the PS Vita / Windows port of **Daytona USA
Deluxe** (SEGA, PC, 1997, v1.20E Direct3D) is built. For installing and
playing, see the main [README](../README.md).

The port is a **whole-program static recompilation**. The game's x86 code was
translated to C once, at development time. It is compiled natively for ARM
(PS Vita) or x86 (Windows), and nothing is emulated at the CPU level. The
original game was written against Windows 95 and DirectX 5, so the port
carries its own implementation of the Win32 and DirectX functions the game
calls, built on OpenGL, the platform's input and audio, and plain files.

The game's data stays the user's: at start-up the port loads
`DAYTONA USA Deluxe.exe` (v1.20E), `ddse.dll`, `Resource.dll`, the `Resource\`
tree and the CD music (`CDDA_BGM\NN.mp3`) from the data folder. The converter
(`tools/converter`) produces that folder from an original CD.

---

## 1. Repository layout

| Path | Contents |
|------|----------|
| `recomp/` | The recompiler. `gen.py` turns the Ghidra export into C; `export/` and `export_ddse/` hold the export; `gen/` and `gen_ddse/` hold the generated C; `lifted/` holds the instruction-lifted 3D engine; `overrides/` holds hand-written replacements; `include/` holds the shared headers (`recomp.h`, `lifted.h`, `crt.h`) |
| `runtime/` | Platform-independent runtime: Win32 and DirectX 5 emulation, the OpenGL renderer, the mixer, the options menu, the profiler |
| `host/` | Windows platform layer (`plat_win.c`) and its build script (`build.py`, MSVC) |
| `vita/` | PS Vita platform layer (`plat_vita.c`), CMake project, `build.sh`, `package.sh` |
| `ghidra/` | Ghidra scripts: `RecompExport.java` (the export that `gen.py` reads), `ListOverrides.java`, `DecompOne.java` |
| `tools/lift_engine.py` | Lifts the 3D engine instruction by instruction (pcrecomp `lift32`, MIT, in `tools/third_party/pcrecomp`) |
| `tools/oracle/` | Validation against the original game: a probe DLL logs car and render records from the real exe, and `cmpcars.py` / `cmpdraws.py` compare them with the port |
| `tools/converter/` | DayConv, the end-user converter (C#, .NET 8, WinForms) |
| `tools/*.py` | Analysis helpers used during the work (disassembly, table dumps, import/string lists, stub generation) |
| `docs/` | This file and the early reverse-engineering notes |

---

## 2. Recompilation pipeline

```
DAYTONA USA Deluxe.exe ──Ghidra──► recomp/export/ ──gen.py──► recomp/gen/*.c ─┐
ddse.dll ───────────────Ghidra──► recomp/export_ddse/ ──gen.py──► gen_ddse/ ─┤
3D engine 0x46c000..0x4a7000 ──lift_engine.py──► recomp/lifted/*.c ─────────┤
hand-written fixes ──────────────────────────────► recomp/overrides/*.c ────┤
runtime/*.c + host/ or vita/ platform layer ─────────────────────────────────┴─► executable
```

### 2.1 Ghidra export

`ghidra/RecompExport.java` runs headless (`run_recomp_export.bat`, about 70 s).
It writes every function's decompiled C, its prototype, the symbols, the code
pointers and Ghidra's data types (`types.h`). Before decompiling, it strips the
call-site overrides Ghidra's printf format analysis puts on `sprintf` calls:
they corrupted stack tracking, which caused a stuck gear HUD and a crash on
the records screen. It also applies the hand-written prototype corrections in
`recomp/fixups*.txt` (some derived by `analyze_fixups.py`,
`tools/find_uninit.py` and `tools/find_esp_patches.py`) before decompiling.
The `.bat` files point at a local Ghidra 12.2 install; adjust the path.

### 2.2 `gen.py`

`gen.py` makes Ghidra's output compile and behave like the original:

- **Memory model.** Every global is an offset into an image that mirrors the
  original 32-bit address space (`VA(a)`, `G(type, a)`; `g_image` for the exe,
  `g_image_ddse` for the DLL). Locals that Ghidra placed in the stack frame
  keep their layout (`_frame[]`), because the code takes their addresses.
- **Unaligned access.** On ARM, multi-byte reads of guest addresses use
  `aligned(1)` types (`recomp_u32u`, `recomp_f32u`, … in
  `recomp_unaligned.h`); otherwise VLDR/LDRD fault on unaligned data.
- **Fake addresses.** Numbers Ghidra printed as addresses
  (`&DAT_00540000` for the value 0x540000) are turned back into numbers
  (`fake_addrs*.txt`, from `tools/find_fake_addrs.py`).
- **Overrides.** Functions listed in `overrides.txt` are not emitted; their
  code is in `recomp/overrides/fixed_00.c`. Some fix Ghidra's stack tracking.
  Others are port changes: the Multiplayer entry skipped on the Vita, the fade
  loops as tight C, the session-only car cheat, the autosave hooks.
- **Imports.** Calls to Win32/DirectX imports become calls to `w32_*`
  functions in the runtime. `tools/gen_stubs.py` generates stubs for any not
  implemented yet.
- **Code pointers.** `functable.c` maps each original function address to its
  recompiled function. Pointer tables in `.data` (scene handlers, callbacks)
  are rebased at load time to point at the recompiled code.

Run `cd recomp && python gen.py` (and `python gen.py --module ddse` for the
DLL).

### 2.3 The lifted 3D engine

Ghidra's decompilation of the 3D engine (0x46c000–0x4a7000: transforms, x87
maths, polygon setup) produced enough errors that nothing was drawn.
`tools/lift_engine.py` translates that range **instruction by instruction**
with pcrecomp's `lift32` into `recomp/lifted/engine_lifted.c`:

- The x86 registers live in locals (`RECOMP_LOCAL_REGS`).
- The x87 stack is a ring with a local top, and `fistp` rounding is done
  inline.
- Memory access goes through `MEM8/16/32` macros.

Port behaviour is inserted at fixed instruction addresses through
`PORT_HOOKS` in `lift_engine.py`. A hook that calls back into engine code is
wrapped in `RECOMP_REGS_OUT()` / `RECOMP_REGS_IN()`. The hooks themselves are
in `runtime/lifted_rt.c`. Regenerate with `python tools/lift_engine.py`; it
reads the exe from `game/`.

---

## 3. Runtime

### 3.1 Start-up (`main.c`, `image.c`)

1. The platform opens the window/GL context, input and audio.
2. The sections of `DAYTONA USA Deluxe.exe` and `ddse.dll` are copied into
   their images and the base relocations are applied. A relocated pointer to
   data is rebased into the image; a pointer to code becomes the host address
   of the recompiled function (48 825 relocations, 894 of them to code).
3. The game's own `WinMain` path is called. From there on it is the original
   program, running against the emulated Win32 and DirectX.

### 3.2 Win32 (`w32_*.c`, `res.c`, `crt.c`)

- **KERNEL32**: files (case-insensitive paths mapped into the data folder),
  memory, time, threads (where needed).
- **USER32/GDI32**: one real window. Message queue, timers and dialogs are
  emulated.
- **Registry**: in memory, saved to `registry.dat` in the data folder.
- **Resources**: string tables and dialogs from the exe image and
  `Resource.dll`.
- **winmm** (the game's `_INMM.dll` wrapper): timer, `mmio` RIFF reading, and
  MCI CD audio played from `CDDA_BGM\NN.mp3`.
- **MSVC 4.2 CRT functions** on the host libc.

### 3.3 DirectX 5

- **DirectDraw** (`dx_ddraw.c`) keeps surfaces in CPU memory. The game's 2D
  (menus, HUD, sprites from the recompiled `ddse.dll`) is drawn into them, and
  only the rectangles the game wrote are uploaded each frame. Background blits
  (the sky panorama copies and fills drawn before the 3D) are queued and drawn
  as GL quads, which is how the sky can extend into the widescreen margins.
- **Direct3D 5 immediate mode** (`dx_d3d.c`) receives pre-transformed (TL)
  vertices and render states and hands them to `render.c`.
- **DirectSound** voices are mixed in software (`audio.c`: linear
  interpolation, soft limiter). The CD music is streamed MP3 (dr_mp3),
  loaded on a background job so the game never stalls on the memory card.
- **DirectInput** reports the platform pad as the game's keyboard and as a
  "SEGA SATURN PAD" joystick profile.

### 3.4 Renderer (`render.c`)

Fixed-function OpenGL 1.x: `opengl32` on Windows, vitaGL on the Vita.

- **Texture atlas** (2048×2048, 2-pixel gutters). The game changes texture on
  almost every polygon; the atlas brought a race from 400–800 draw calls per
  frame down to about 50–100. This was the step that reached 30 FPS on the
  Vita.
- **Batching**: draws are merged until state changes (`rnd_flush`).
- **2D layer**: the game's 640×480 frame is shown over the 3D through rotating
  16-bit textures written in place (`vglGetTexDataPointer` on the Vita).
- **Screen layout**: the picture keeps the Vita's 960:544 shape (or 4:3 with
  widescreen off), centred in a window of any size.

### 3.5 Platform layer (`plat.h`)

`plat_vita.c` / `plat_win.c` supply only: GL context, swap, pad, a 44.1 kHz
stereo audio callback, time, a background thread and the data directory. The
Windows layer adds window modes (1×–4×, borderless, Alt+Enter) and key
bindings.

---

## 4. What the port adds to the game

### Widescreen

- **3D view.** The engine projects into a 352×224 view: half sizes 176×112 in
  the camera blocks `0x4e94e4`, `0x56ec00` (near pass) and `0x56f000` (far
  pass). The port widens the half width to 233, which is 16:9 at the same
  height, and `render.c` spreads the 3D over the whole screen. Split-screen
  and the name entry / records scenes (42, 43) stay 4:3.
- **Polygon side limit.** The transform at 0x470089 drops near polygons
  beyond the side limit `[0x4e94c4]`. Widened by 233/176 (`lifted_side_limit`).
- **Sky.** The panorama copies extend into the margins. The 931×512
  panorama is not seamless, so past either end it continues mirrored. When the
  game copies only a band (Sea-Side Street Galaxy), the band continues
  mirrored downwards.
- **HUD.** While the race HUD is up, its side parts (lap times, position,
  condition box, course map) move to the screen edges: 2D rectangles by zone
  in `rnd_hud_side`, D3D HUD quads (marked by rhw 1e-5) in `dx_d3d.c`.
- **Fades.** Fades and the options menu also darken the margins.

### Draw distance

- **Block passes.** The course is drawn as consecutive blocks. Near blocks
  are drawn by 0x47bbe8; far blocks by 0x47bd70, through a second "CPU
  context" (0x44bd79) with a 4-corner cull (0x47e22d).
- **What the port changes.** It adds far blocks ahead (`lifted_far_extend`)
  and fixes the corner cull for blocks whose corners are off screen on
  different sides (`lifted_corner_*`), which had made the road vanish.
- **Adaptive.** The extra blocks follow the far polygon buffer's fill and the
  frame time, so the buffer never overflows: an overflow crashed the replay.
- **What must not change.** The visible-block flags (`0x4ea048`) and the car
  level of detail (car+0x96) also drive the AI simulation. Changing them makes
  the cars diverge from the original. Every engine change was checked with
  `tools/oracle/cmpcars.py`: identical car positions over about 1980 frames of
  the attract demo.

### Performance (Vita, 30 FPS like the original)

- Texture atlas and draw batching.
- 16-bit frame textures written in place.
- Only the 2D rectangles the game wrote are uploaded.
- Background blits on the GPU.
- Async MP3 loading (there was a 1.5 s freeze at every track change).
- Fade loops and the fade lookup table rewritten in tight C.
- Draw distance adaptive to frame time.

### Options menu (`menu.c`; Select on the main menu)

- **Pages**: DISPLAY, AUDIO (BGM/SFX 0–200 %), ARCADE, TIME ATTACK and
  1P VS 2P (the PC version's game-settings dialog), KEYS (Windows), ABOUT.
- **Windows extras**: WINDOW (1×–4×, fullscreen).
- **Storage**: settings are kept under `Software\SEGA\DAYTONA USA Deluxe\Port`
  in `registry.dat`.
- **Hint**: a reminder of the Select button is drawn in the main menu's
  right black bar.

### Saves

- **Autosave.** The game saves records and settings only when quitting
  through EXIT (`FUN_004325e4`). The port also saves after the name entry and
  the records screen.
- **Car cheat (session only).** It unlocks the extra cars (`0x89ce5c`) for
  the current session only. The save keeps the real count, and the game's own
  unlock progress keeps counting on it (overrides of `FUN_0045b030` and
  `FUN_0040d493`).

---

## 5. Building

### Windows (test platform and PC release)

- Requirements: Visual Studio 2022 (x86 C toolchain) and Python 3. Set the
  `vcvars32.bat` path in `host/build.py` if yours is elsewhere.
- Command: `python host/build.py` → `build_host/daytona.exe`, an incremental
  parallel build of about 60 units.
- Data folder: `DAYTONA_DATA`, else `data\dayusadx\` next to the exe, else the
  exe's own folder, else `..\game\`.

### PS Vita

- Requirements: VitaSDK with vitaGL (the softfp toolchain), built in WSL or
  Linux.
- Commands:

  ```sh
  export VITASDK=/path/to/vitasdk-softfp
  bash vita/build.sh      # CMake -> vita/build/daytona_vita
  bash vita/package.sh    # -> vita/dayusadx.vpk (title ID DAYUSADX0, eboot only)
  ```

- The VPK enables extended memory (`ATTRIBUTE2=12`). The main thread has a
  2 MB stack.
- Data folder: `ux0:data/dayusadx/`.

### Regenerating the C

Needs the game files in `game/` (v1.20E exe, `ddse.dll`):

```
ghidra\run_recomp_export.bat 0        (and run_recomp_export_ddse.bat)
cd recomp && python gen.py && python gen.py --module ddse
python tools/lift_engine.py
```

### Converter

```
python tools/converter/make_patch.py <extracted CD root> game tools/converter/DayConv/patch.bin
cd tools/converter/DayConv && dotnet publish -c Release -o ../publish
```

See [tools/converter/README.md](../tools/converter/README.md).

---

## 6. Debugging

- **Log**: `daytona_log.txt` in the data folder. It lists scene changes, CD
  tracks, autosaves, and a profiler line every 4 s (`prof … slow N max M ms`
  and the top functions).
- **Vita crashes**: run `vita-crashdump <dmp> vita/build/daytona_vita`, then
  `arm-vita-eabi-addr2line` at 0x81000000 + offset.
- **Windows crashes**: a symbolised stack goes to the log (needs
  `daytona.pdb`).
- **Windows console**: the exe is a windowed program; run it with
  `--console` to open a console that shows the log as it is written.
- **Environment variables** (Windows build, `DAYTONA_…`):

  | Variable | Use |
  |----------|-----|
  | `DATA` | data folder |
  | `WINMODE`=0..4 | window mode (1×–4×, fullscreen) |
  | `WIDE`=0/1, `DRAWDIST`=0..3 | override the options |
  | `GOTO`=sec:scene | jump to a scene (e.g. 42 name entry, 43 records) |
  | `KEYS`="from-to:mask;…" | scripted pad input (seconds, hex `PAD_` bits) |
  | `SHOT`=N, `SHOT_FROM` | save a BMP every N frames |
  | `MENUTEST`, `CHEATCARS`, `CHEATSAVE` | open the menu / test the car cheat |
  | `CARDUMP`, `D3DSTATS` | per-frame car and draw records for the oracle tools |
  | `BLKLOG`, `PVSLOG`, `CLIPLOG`, `FILLLOG`, `CARFLICK` | draw-distance and engine logs |
  | `HUDLOG`, `BGLOG`, `FADELOG`, `BLTSTAT`, `FLUSHSTATS` | 2D, HUD, fade and batching logs |
  | `NOATLAS`, `NOBGGL`, `NO3D` | turn off the atlas / GPU background / all 3D polygons |
| `CHEATCAR`=n | with `CHEATCARS`, the arcade car cursor starts on car n (10 = Uma 2) |
  | `WATCH`=addr (+`_RING`, `_EQ`, …) | hardware write watchpoint on a game address |
  | `SAMPLE`=ms | sampling profiler with call stacks |
  | `WAVDUMP`, `MIXREC` | audio dumps |

---

## 7. Useful addresses (v1.20E)

| Address | Meaning |
|---------|---------|
| `0x866684` | current scene; table at `0x4be740`, `SEQUENCE_*` names at `0x4b9d84` (4 main menu, 6 course select, 10 car select, 28 race, 36 goal, 42 name entry, 43 records, 48 replay); post a change with `FUN_0045ac75(0x14, scene, 0)` |
| `0x89c6a0`..+`0xe94` | settings block, saved as `DAYTONA_PROTECTED_OBJECT` |
| `0x89cdd8` / `0x89cddc` | game mode / difficulty |
| `0x89ce5c` / `0x89cee4` | extra cars unlocked (0..3) / wins mask behind the unlocks |
| `0x89ce1c` / `0x89ce20` | car picked, player 1 / 2 |
| `0x89cee8` | display flags (bit 0 background, 1 mph, 3 high polygon) |
| `0x8626e8` | race timer (seconds, −1 every 30 frames) |
| `0x4e94e4` | camera block (half sizes at +0x18 / +0x1a) |
| `0x5172cc` | car list (level of detail at +0x96) |

---

## 8. Known limitations

- **Multiplayer** (DirectPlay over a network) is not available; 1P vs 2P
  split screen is.
- **No force feedback.**
- **NPC cars can blink** near or beside the camera in 16:9. This is the
  original visibility logic: cars outside the original 4:3 view are not
  drawn. Changing it changes the AI.
- **Distant scenery pop-in** is an engine limit (consecutive-block drawing).
  It is reduced, not removed.
- **Name entry and records** stay 4:3.

---

## 9. Third-party code

- **pcrecomp** `lift32` (MIT, sp00nz): `tools/third_party/pcrecomp`
- **dr_mp3** (public domain / MIT-0): `runtime/dr_mp3.h`
- **vitaGL** and **VitaSDK**, used to build the Vita version
- **NAudio** and **NAudio.Lame / LAME** (LGPL), used by the converter
