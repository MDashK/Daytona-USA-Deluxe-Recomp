# Daytona USA Deluxe — Reverse Engineering Notes

Reverse-engineering documentation for **Daytona USA Deluxe** (PC, v1.20E),
a Sega AM2 / AM#2 racing game from 1997. Goal: reconstruct portable,
re-compilable source with a view to porting to the PS Vita (VitaSDK + VitaGL).

## Binary

| Field            | Value |
|------------------|-------|
| File             | `DAYTONA USA Deluxe.exe` (1,096,704 bytes) |
| Format           | PE32, x86 (i386) |
| Compiler/linker  | **Microsoft Visual C++ 4.2** (linker version 4.20) |
| Entry point      | VA `0x004A87C0` (CRT `mainCRTStartup`, end of `.text`) |
| Image base       | `0x00400000` |
| Subsystem        | Windows GUI (2) |
| Packed?          | No (sections map cleanly, plain MSVC CRT) |
| Version resource | `0.1.2.0` |

Sections:

| Name    | VA       | VSize   | Notes |
|---------|----------|---------|-------|
| `.text` | 0x401000 | 0xACE50 | ~700 KB code |
| `.rdata`| 0x40AE000| 0x56D3  | read-only data / strings |
| `.data` | 0x40B4000| 0x3EAC30| data + large BSS |
| `.idata`| 0x449F000| 0x164A  | import table |
| `.rsrc` | 0x44A1000| 0x4270  | resources (dialogs, menu, version) |
| `.reloc`| 0x44A6000| 0x1AE50 | relocations |

### The No-CD crack

The distributed `.exe` is the Wraithverge No-CD crack of the official
"DDX_D3D.ZIP" v1.20E Direct3D patch. It differs from the original by **exactly
8 bytes** at VA `0x0044AB12` (file offset `0x49F12`):

```
85 C0 0F 84 19 00 00 00   test eax, eax ; je <cd_error>
→ 90 90 90 90 90 90 90 90   (8× NOP)
```

`game/DAYTONA USA Deluxe.org` is the unpatched original. The CD check itself
lives partly in `DDSE.dll` (a single exported symbol `DdseFunc`), which is
called from the main exe and also checks for the CD.

## Runtime platform (DirectX 5 era, 1997)

The game targets the DirectX 5 SDK stack:

- **DirectDraw** (`ddraw.dll`) — `DirectDrawCreate`, `DirectDrawEnumerateA`.
  Primary-surface 2D plus Direct3D via `QueryInterface` on the DirectDraw COM
  object (no separate `Direct3DCreate` import), 16-bit and 8-bit modes.
- **Direct3D** — the v1.20E patch adds D3D hardware acceleration
  (320×240 and 640×480, 16-bit full-screen). Software DirectDraw remains.
- **DirectSound** (`dsound.dll`) — `DirectSoundCreate` (SFX).
- **DirectInput** (`dinput.dll`) — `DirectInputCreateA` (keyboard, joystick,
  steering wheel), plus **force feedback** (the "VFX1.0" CLSID
  `{04ace0a7-1fa8-11d0-aa22-00a0c911f471}` and VFX error string table).
- **DirectPlay** (`dplayx.dll`, ordinals 1 & 4) — network/2-player multiplayer,
  wrapped by Sega's `Dpctrl.dll`.
- **CD audio** (`_inmm.dll`) — redbook/CDDA playback via MCI
  (`mciSendString`: `cdda_init()`, `cd_play()`, `cd_pause()`, `cd_stop()`,
  `_CDPlay()`). `CDDA_BGM/` holds the tracks (converted to mp3).
- **MSACM32** (`acmMetrics`) — audio compression manager (WAV playback).

### Game DLLs (all in the game directory)

| DLL | Role |
|-----|------|
| `Dpctrl.dll` | Multiplayer lobby: `DPConnectSettingWizard`, `ConnectDirectPlay`, `CreateDirectPlayChatRoom`, `SendDirectPlayMessage`, pad sync (`_SyncPad`, `_SendDataG`, `_GetPosData`, `_ReceiveData`) |
| `DDSE.dll` | Single export `DdseFunc` — CD/anti-tamper check (`DAYTONA_PROTECTED_OBJECT` marker string lives in the exe) |
| `DXError.dll` | `DDErrorToResourceID`, `DIErrorToResourceID`, `DPErrorToResourceID`, `DSErrorToResourceID` — error-code → resource-string mapping |
| `Resource.dll` | UI resources (dialogs: Direct3D options, etc.) |
| `_INMM.dll` | Media I/O: `_mioOpenA`, `_mioRead`, `_mioSeek`, `_ciSendCommandA`, `_oyGetNumDevs`, `_oySetCapture`, `_oyGetPosEx` — CD audio/MIDI playback |
| `DPCTRL.dll`, `WINCPUID.DLL` | Multiplayer control; CPUID helper |

## Code architecture (from leaked strings)

The project was built from a `D:\day2\Src\` tree. Five source files leak their
path via `__FILE__` in assertions:

- `D:\day2\Src\Appmem.c` — memory manager (`HeapFree: error.`)
- `D:\day2\Src\appevent.c` — application event loop
- `D:\day2\Src\Ddutils.c`, `D:\day2\Src\Dd2utils.c` — DirectDraw utilities
- `D:\day2\Src\Game.c` — main game

### Object model (C++/COM-style interfaces)

The engine exposes an interface tree, evidenced by `__FILE__`-free assert
messages of the form `!! BUG !! IXxx_Method()`:

- `IScene` — 3D scene graph: `IScene_Create`, `IScene_GetAttachedObject`,
  `IScene_Duplicate`
- `ISceneObject` — `ISceneObject_Create`
- `ISceneEffect` — `ISceneEffect_Create`
- `ISceneBitmap` — `ISceneBitmap_Create`, `ISceneBitmap_GetObject`
- `ISceneText` — `ISceneText_Create`
- `IStage` — 2D compositing/font layer: `IStage_Create`, `IStage_DrawText`,
  `IStage_CreateEffect`, `IStage_CreateCompatibleBitmap`

`Stage` = 2D UI/fonts/sprites; `Scene` = 3D world (courses, cars, effects).

### Game flow (state machine)

`SEQUENCE_*` constants map the whole attract → title → mode → race → result
flow. Suffixes: `_A` = Arcade, `_T` = Time Attack, `_2` = 2-Player Battle,
`_L` = (LAN/network) multiplayer.

```
SEQUENCE_BOOT, SEQUENCE_LOGO, SEQUENCE_ADVERTISE, SEQUENCE_TITLE,
SEQUENCE_MODESELECT, SEQUENCE_RECORDS,
SEQUENCE_COURSESELECT_{A,T,2,L}, SEQUENCE_CARSELECT_{A,T,2,L},
SEQUENCE_CARSETTING_{A,T,2,L}, SEQUENCE_SELECTLAPS_{T,2,L},
SEQUENCE_DELAYTIME_{2,L}, SEQUENCE_SLOWCARBOOST_{2,L}, SEQUENCE_TIMELAG_{2,L},
SEQUENCE_RACESTART, SEQUENCE_RACE_{A,T,2,L},
SEQUENCE_GAMEOVER_{A,T,2,L}, SEQUENCE_GOAL_{A,T,2,L},
SEQUENCE_ENDING_{0,1}, SEQUENCE_NAMEENTRY,
SEQUENCE_RESULT_{A,T,2,L,F}, SEQUENCE_REPLAY_{A,T}
```

### Content & file formats (from format-string references)

Six courses (classic Daytona tracks) and the playable car list:

- Courses: `Three Seven Speedway`, `Dinosaur Canyon`,
  `Sea-Side Street Galaxy`, `National Park Speedway`, `Desert City`,
  `Silver Ocean Causeway` (plus extra/mirror layouts `bgEX`, `Extra0/1`).
- Cars: `Hornet`, `Gallop`, `Phoenix`, `Oriole`, `Magic`, `Balance`.

Per-course data files (`x` = course index, referenced with `COURSE%d.MDL` etc.):

- `COURSEx.MDL` / `COURSE%d.TEX` — course model + texture
- `CSx_2.MDL` / `CS%d_2.TEX` — second course segment model
- `CSx_1.BLK`, `CSx_2.BLK`, `CSx_BLK.BIN` — block/collision data
- `CSx_COL.BIN`, `CS12_COL.BIN` — collision
- `OTLINEx.BIN` — out-of-track / track-limit line
- `CSx.BG` — background
- `CS%dOBJ.MDL` / `.TEX` — per-course objects (`resource\t8\obj\%d`)
- `CAREFC.MDL` / `.TEX` — the player car (Hornet); `PLCAR%02d` = player car,
  `EMCAR%02d` = enemy car (`resource\t8\plcar`, `resource\t8\emcar`)
- `SCREEN.BIN`, `CMVDATA.BIN` — title/attract screens, movie data
- `*.gst` — replay ghosts (`Ghost\%s.gst`)

Save data (serialized into a memory object): `Memory`, `arcade_rank`,
`time_attack_rank`, `lap_time_rank`, `block_time_rank`, `fastest_sect_lap`,
`SerializeMemories: `.

### Registry / configuration

- `Software\SEGA\DAYTONA USA Evolution` (profile settings)
- `Software\SEGA\DAYTONA USA Deluxe`
- `Software\SEGA\OEMJoystickSettings`
- Profile keys: `FogEnabled`, `D3DOPTIONS`, `PerspectiveCorrectionEnabled`,
  `FilterEnabled`, `FogDepth`, `DebugFlags`, `Hidden`, `DisplayMode`, `System`.

Supported controllers include `SEGA SATURN PAD`, `Microsoft SideWinder game
pad`, `Microsoft SideWinder Force Feedback Pro`, and `NVidia Sega Arcade
Racer` / `Mission Stick` variants.

### Runtime / framework details

- MSVC 4.2 static CRT (R6002–R6028 runtime error strings).
- Single-instance mutex ("Multiple Instance Error.").
- Win32 GUI with a menu bar (`F3` toggles pseudo-window mode); many dialog
  boxes; `WM_*` message-name table present for a debug message dumper.
- `OnPaint`, `OnExit`, `Setup` handler names; `FixedSys` font.

## Roadmap

1. **Decompile** — dump Ghidra decompilation of every function (in progress).
2. **Structure** — organise functions into modules (`Appmem.c`, `Game.c`,
   `appevent.c`, `Ddutils.c`, `Dd2utils.c`, scene/stage engine, race, input,
   audio, UI), name them from the string/format-string cross-references.
3. **PAL (platform abstraction layer)** — define the interface that the PC
   side implements with DirectDraw/DirectSound/DirectInput/DirectPlay and the
   Vita side implements with VitaGL/VitaSDK. This is the port's seam.
4. **Make it compile** — re-type decompiled code against the PAL + MSVC4.2-CRT
   shims so a host build links and runs.
5. **Port to PS Vita** — implement the PAL on VitaSDK/VitaGL; replace
   DirectDraw/D3D with VitaGL, DirectSound with Vita audio, DirectInput with
   Vita controls, DirectPlay with local/Vita net; CDDA → mp3 playback.
