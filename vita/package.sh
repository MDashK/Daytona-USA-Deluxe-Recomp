#!/bin/bash
# Package the Vita homebrew as a .vpk (eboot only). Run inside WSL.
#
# The game data is NOT bundled in the vpk.  On the device copy the whole
# folder of the PC game (version 1.20E) to ux0:data/dayusadx/ :
#   ux0:data/dayusadx/DAYTONA USA Deluxe.exe   (code/data image, v1.20E)
#   ux0:data/dayusadx/ddse.dll
#   ux0:data/dayusadx/Resource.dll
#   ux0:data/dayusadx/Resource/                (models, textures, sounds...)
#   ux0:data/dayusadx/CDDA_BGM/                (the CD tracks as MP3)
# Settings (registry.dat) and the log (daytona_log.txt) are written there too.
#
# Flow: ELF -> vita-elf-create (relocatable) -> vita-make-fself (FSELF)
#              -> vita-pack-vpk (.vpk).
set -e

export VITASDK="${VITASDK:-/home/mdashk/vitasdk-softfp}"
cd "$(dirname "$0")"

mkdir -p vpk
cd vpk
test -f ../build/daytona_vita && test ../build/daytona_vita -nt ../dayusadx.vpk -o ! -f ../dayusadx.vpk || { echo "ELF older than the vpk: build failed?"; exit 1; }

# ATTRIBUTE2=12: extended memory mode (+109 MB)
"$VITASDK/bin/vita-mksfoex" -s TITLE_ID=DAYUSADX0 -d ATTRIBUTE2=12 "Daytona USA Deluxe" param.sfo
"$VITASDK/bin/vita-elf-create" ../build/daytona_vita daytona.velf
"$VITASDK/bin/vita-make-fself" -c daytona.velf eboot.bin
"$VITASDK/bin/vita-pack-vpk" -s param.sfo -b eboot.bin ../dayusadx.vpk

echo
echo "Built ../dayusadx.vpk (eboot only)."
echo "Copy the PC game folder (v1.20E) to ux0:data/dayusadx/ on the device."
