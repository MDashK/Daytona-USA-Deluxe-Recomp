# Daytona USA Deluxe for PS Vita and Windows

A native port of **Daytona USA Deluxe** (SEGA, PC, 1997) to the **PlayStation
Vita**, with a Windows version from the same code. It is not an emulator: the
game's code was recompiled to run natively.

**You need your own original Daytona USA Deluxe CD.** No game files are
included here. The converter included in the release makes them from your
disc.

## Features

- Full game: Arcade, Time Attack, 1P vs 2P (not yet tested), Records, replays,
  every course and time of day of the v1.20E update
- 30 FPS, as on the original PC version (there's a reason for this. For more info, keep reading)
- **Widescreen 16:9**: the 3D fills the screen, the sky extends to the edges,
  and the race HUD moves to the screen edges
- **Longer draw distance** than the original
- CD music from your disc (as MP3)
- **Options menu**: press **Select** on the main menu
  - Display: speed unit, polygons, background, fog, texture filter
  - Audio: music and sound effects volume (0–200 %)
  - Arcade / Time Attack / 1P vs 2P: game mode (Normal, Grand Prix,
    Endurance), difficulty, time compare, ghost car, which HUD items show
  - Windows only: window size (1×–4×, fullscreen) and key bindings
- Records and settings are saved automatically

## What you need

**To convert your game (once):**

- A Windows PC with the [.NET 8 Desktop Runtime](https://dotnet.microsoft.com/download/dotnet/8.0)
- Your **Daytona USA Deluxe CD**, as a disc in the drive or as a BIN/CUE image
  of it. Any region or publisher works, with excpetion for the Japanese release (Evolution).

**For the PS Vita:**

- A PS Vita / PS TV with homebrew enabled (HENkaku / Ensō) and VitaShell
- `libshacccg.suprx` installed (it is needed by most vitaGL games; if
  you don't have it, install it with ShaRKBR33D)
- About 160 MB free on `ux0:`

**For Windows:** any PC with OpenGL graphics.

## Installation

### 1. Make the game files from your CD

1. Download the latest release and unzip it.
2. Run `DayConv.exe` (in `Converter`).
3. Under **Game CD / image**, choose one of:
   - the `.cue` file of your BIN/CUE image (**recommended**: it includes the
     music), or
   - `DAYTONA USA Deluxe.exe` in the `GAME` folder on your CD.
4. The converter checks that it is the original game, then shows *"Original
   CD exe found"* in green.
5. Choose an output folder and press **Convert**. It takes about a minute.

The converter copies the game files, updates them to version 1.20E (Direct3D)
and converts the CD music to MP3. When it finishes, the output folder holds a
`data\dayusadx` folder.

> An `.iso` image has no CD music in it. The game runs, but without music.
> Use BIN/CUE or the disc itself.

### 2a. PS Vita

1. Copy `dayusadx.vpk` (in `PSVita`) to the Vita and install it with
   VitaShell.
2. Copy the **`data`** folder from the converter's output folder to the root
   of `ux0:`. The game files must end up in `ux0:data/dayusadx/`, where you
   should see `DAYTONA USA Deluxe.exe`, `Resource`, `CDDA_BGM` and the rest.
3. Start **Daytona USA Deluxe** from the LiveArea.

### 2b. Windows

1. Copy `DaytonaUSADX.exe` (in `Windows`) into the converter's output folder,
   next to its `data` folder.
2. Run `DaytonaUSADX.exe`.

## Controls

| Action | PS Vita | Windows (default keys) |
|--------|---------|------------------------|
| Steer | Left stick / D-pad | ← → |
| Accelerate | Cross | X |
| Brake | Square (or Circle) | Z |
| Shift up / down | R / L (or D-pad ↑ / ↓) | S / A (or ↑ / ↓) |
| Change view | Triangle | C |
| Start / confirm | Start | Enter |
| Back | Circle | Esc |
| Options menu (on the main menu) | Select | Backspace |
| Fullscreen | — | Alt+Enter |

On Windows, every key can be changed in the options menu (KEYS page).

## Saves and settings

- **PS Vita:** records and settings are kept in `ux0:data/dayusadx/registry.dat`.
- **Windows:** they are kept in `data\dayusadx\registry.dat`.
- **When it saves:** after you enter your name and after the records screen,
  and when you quit through **EXIT**.
- **Log:** `daytona_log.txt` in the same folder is useful when reporting a
  problem.

## Troubleshooting

- **"Game files not found"**: the `data\dayusadx` folder is missing or in the
  wrong place. On the Vita it must be `ux0:data/dayusadx/`.
- **The converter says "Not the original CD exe"**: it needs the files from
  the original CD. Files from an already patched or cracked copy can't be
  used.
- **No music**: the conversion was made from an `.iso`, or from a copy without
  the CD audio tracks. Convert again from the BIN/CUE or the disc.
- **The game doesn't start on the Vita**: check that `libshacccg.suprx` is
  installed.

## Known limitations

- Online/LAN multiplayer is not available.
- 1P vs 2P split screen is, but it's untested for now.
- Opponent cars can briefly disappear when they are beside you in
  widescreen. This is how the original game decides which cars to draw.
- The name entry and records screens stay in 4:3.

### Why 30 FPS?

The game was built to run at 30 frames per second, and its logic is tied to that. The original PC version (1997) already ran at 30 FPS. Each frame isn’t just a picture: it’s also one step of the game. In each step the cars move a fixed amount, the AI decides, the physics advances, and the clock counts. The race timer, for example, drops one second every 30 frames.

If the game ran 60 steps per second, everything would happen twice as fast: cars, opponents, timer, music synced to events. It would be the game in fast-forward, not smoother.

To get real 60 FPS you’d have to separate the game from the picture. The game would keep advancing 30 times per second, and the image would be drawn 60 times, inventing the in-between positions (interpolation). That means rewriting how the original 3D engine draws everything, which is a big job and risky for the game’s behaviour.

And on the Vita there’s no room for it anyway. At 30 FPS the console already spends most of each frame running the game and drawing it. Drawing twice as many frames wouldn’t fit.

In one sentence: The game counts time in frames: at 60 FPS it would run at double speed. Doing proper 60 FPS would mean rewriting the original engine, and the Vita already uses nearly all its power at 30.

## Credits

- Port by **MDashK**
- AI assisted by **Claude** (Anthropic)
- Daytona USA Deluxe © SEGA. This project is not affiliated with or endorsed
  by SEGA. It contains no game data; you need your own original copy of the
  game.
- Third-party code:
  - [pcrecomp](tools/third_party/pcrecomp) (MIT)
  - dr_mp3 (public domain / MIT-0)
  - vitaGL and VitaSDK
  - NAudio and LAME (converter)

How the port works and how to build it: see
[docs/TECHNICAL.md](docs/TECHNICAL.md).
