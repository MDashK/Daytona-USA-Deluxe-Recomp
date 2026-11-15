@echo off
rem Decompile the given addresses (hex) with callee information -> recomp\one.txt
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
call "E:\_ROF\tools\ghidra_12.2_PUBLIC\support\analyzeHeadless.bat" "%ROOT%\ghidra_recomp" DaytonaRecomp -process "DAYTONA USA Deluxe.exe" -noanalysis -readOnly -scriptPath "%ROOT%\ghidra" -postScript DecompOne.java "%ROOT%\recomp\one.txt" %* > "%ROOT%\recomp\one_log.txt" 2>&1
