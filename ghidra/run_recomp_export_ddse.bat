@echo off
rem Export DDSE.dll (the game's 2D sprite engine) as re-compilable C.
rem Output: recomp\export_ddse\  (same format as run_recomp_export.bat)
setlocal
set "GHIDRA_HOME=E:\_ROF\tools\ghidra_12.2_PUBLIC"
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
set "PROJECT_DIR=%ROOT%\ghidra_recomp"
set "BINARY=%ROOT%\game\ddse.dll"
set "OUT=%ROOT%\recomp\export_ddse"
set "LOG=%ROOT%\recomp\export_ddse_log.txt"
set "FIX=%ROOT%\recomp\fixups_ddse.txt"
if not exist "%FIX%" echo # DDSE.dll fixups> "%FIX%"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%PROJECT_DIR%" mkdir "%PROJECT_DIR%"
if exist "%PROJECT_DIR%\DaytonaRecomp.rep\idata\ddse_imported.flag" (
  call "%GHIDRA_HOME%\support\analyzeHeadless.bat" "%PROJECT_DIR%" DaytonaRecomp ^
    -process "ddse.dll" -noanalysis ^
    -scriptPath "%ROOT%\ghidra" -postScript RecompExport.java "%OUT%" 1 "%FIX%" > "%LOG%" 2>&1
) else (
  call "%GHIDRA_HOME%\support\analyzeHeadless.bat" "%PROJECT_DIR%" DaytonaRecomp ^
    -import "%BINARY%" ^
    -scriptPath "%ROOT%\ghidra" -postScript RecompExport.java "%OUT%" 2 "%FIX%" > "%LOG%" 2>&1
  echo done> "%PROJECT_DIR%\DaytonaRecomp.rep\idata\ddse_imported.flag"
)
echo exit %ERRORLEVEL%
endlocal
