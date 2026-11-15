# DayConv — PS Vita data converter

Windows tool (C#, .NET 8, WinForms) that builds the port's data folder from
the user's own **Daytona USA Deluxe** CD.

Input (Browse or drag and drop):

- `.cue` (with its `.bin` tracks) — game files **and** CD music
- a `.bin` with a `.cue` next to it (used automatically)
- `.iso` / lone `.bin` — game files only (an ISO has no CD audio)
- `GAME\DAYTONA USA Deluxe.exe` on the CD drive — files from the disc, music
  read raw from the drive (`IOCTL_CDROM_RAW_READ`); from a folder on a hard
  disk the files are used but there is no music

Steps:

1. The exe must be the one pressed on the original CDs
   (md5 `c1e334a15d9472219441ed5f2bb89fb1`, every region and publisher).
2. The CD's `GAME` folder goes to `<output>\data\dayusadx\`.
3. The v1.20E Direct3D update is applied: the exe, `Resource.dll` and
   `DPCTRL.dll` from deltas against the CD's files, plus the nine new
   `Resource\640X480X16\MODE\*.BMP`. Each result is md5-checked. The exe comes
   out as the exact file the port was built with
   (md5 `ee476877f27b8c77905033726b9704af`).
4. CD tracks 2–24 are encoded to `CDDA_BGM\NN.mp3` (LAME via NAudio.Lame).

The user then copies `data` to the root of `ux0:`.

## Building

The patch data is made from the game files, so it is not in the repository:

    python tools/converter/make_patch.py <extracted CD root> game tools/converter/DayConv/patch.bin
    cd tools/converter/DayConv
    dotnet publish -c Release -o ../publish

`<extracted CD root>` is the disc's file system (the folder holding `GAME\`).
The result is one `DayConv.exe` (needs the .NET 8 Desktop Runtime).
`DayConv.exe --cli <input> <output>` converts without the window.

`app.ico` is the original exe's icon (`extract_icon.py`).
