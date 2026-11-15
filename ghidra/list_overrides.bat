@echo off
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
call "E:\_ROF\tools\ghidra_12.2_PUBLIC\support\analyzeHeadless.bat" "%ROOT%\ghidra_recomp" DaytonaRecomp -process "DAYTONA USA Deluxe.exe" -noanalysis -readOnly -scriptPath "%ROOT%\ghidra" -postScript ListOverrides.java "%ROOT%\recomp\overrides_list.txt" %* > "%ROOT%\recomp\overrides_log.txt" 2>&1
