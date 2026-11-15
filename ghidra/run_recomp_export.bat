@echo off
rem Export the whole game as re-compilable C (see ghidra\RecompExport.java).
rem Output: recomp\export\functions.txt, types.h, symbols.txt, code_pointers.txt
rem Usage: run_recomp_export.bat [commitPasses]   (2 on a fresh project, 0 afterwards)
setlocal
set "GHIDRA_HOME=E:\_ROF\tools\ghidra_12.2_PUBLIC"
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
set "PROJECT_DIR=%ROOT%\ghidra_recomp"
set "BINARY=%ROOT%\game\DAYTONA USA Deluxe.exe"
set "OUT=%ROOT%\recomp\export"
set "LOG=%ROOT%\recomp\export_log.txt"
set "PASSES=%1"
if "%PASSES%"=="" set "PASSES=0"

if not exist "%OUT%" mkdir "%OUT%"
set "FIX=%ROOT%\recomp\fixups.txt"
copy /y "%ROOT%\recomp\fixups_auto.txt" "%FIX%" >nul
if exist "%ROOT%\recomp\fixups_manual.txt" type "%ROOT%\recomp\fixups_manual.txt" >> "%FIX%"
if exist "%ROOT%\recomp\fixups_patches.txt" type "%ROOT%\recomp\fixups_patches.txt" >> "%FIX%"
if not exist "%PROJECT_DIR%" mkdir "%PROJECT_DIR%"

if exist "%PROJECT_DIR%\DaytonaRecomp.gpr" (
  call "%GHIDRA_HOME%\support\analyzeHeadless.bat" "%PROJECT_DIR%" DaytonaRecomp ^
    -process "DAYTONA USA Deluxe.exe" -noanalysis ^
    -scriptPath "%ROOT%\ghidra" -postScript RecompExport.java "%OUT%" %PASSES% "%FIX%" > "%LOG%" 2>&1
) else (
  call "%GHIDRA_HOME%\support\analyzeHeadless.bat" "%PROJECT_DIR%" DaytonaRecomp ^
    -import "%BINARY%" ^
    -scriptPath "%ROOT%\ghidra" -postScript RecompExport.java "%OUT%" 2 "%FIX%" > "%LOG%" 2>&1
)
echo exit %ERRORLEVEL%
endlocal
