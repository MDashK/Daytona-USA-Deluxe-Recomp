@echo off
rem Windows x86 (32-bit) test build of the recompiled game, using MSVC.
rem The game code requires 32-bit pointers, so this must be the x86 toolchain.
rem   host\build_host.bat            build build_host\daytona.exe
rem   host\build_host.bat gen        compile only the generated game code (error check)
setlocal enabledelayedexpansion
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
call "E:\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 ( echo vcvars32.bat not found & exit /b 1 )
set "OUT=%ROOT%\build_host"
if not exist "%OUT%" mkdir "%OUT%"
set CFLAGS=/nologo /c /O1 /Oy- /Zi /W2 /wd4002 /wd4003 /wd4018 /wd4024 /wd4028 /wd4047 /wd4133 /wd4244 /wd4267 /wd4305 /wd4306 /wd4311 /wd4312 /wd4700 /wd4715 /wd4716 /wd4723 /wd4029 /wd4087 /D_CRT_SECURE_NO_WARNINGS /I"%ROOT%\recomp\include" /I"%ROOT%\recomp\gen" /I"%ROOT%\runtime"
set ERR=0
for %%f in ("%ROOT%\recomp\gen\*.c") do (
  cl %CFLAGS% "%%f" /Fo"%OUT%\%%~nf.obj" /Fd"%OUT%\vc.pdb" > "%OUT%\%%~nf.log" 2>&1
  if errorlevel 1 ( set ERR=1 & echo FAIL %%~nf ) else ( echo ok   %%~nf )
)
if "%1"=="gen" exit /b %ERR%
python "%ROOT%\tools\gen_stubs.py"
for %%f in ("%ROOT%\runtime\*.c" "%ROOT%\host\*.c") do (
  cl %CFLAGS% "%%f" /Fo"%OUT%\rt_%%~nf.obj" /Fd"%OUT%\vc.pdb" > "%OUT%\rt_%%~nf.log" 2>&1
  if errorlevel 1 ( set ERR=1 & echo FAIL %%~nf ) else ( echo ok   %%~nf )
)
if %ERR%==1 exit /b 1
link /nologo /DEBUG /SUBSYSTEM:CONSOLE /OUT:"%OUT%\daytona.exe" "%OUT%\*.obj" user32.lib gdi32.lib opengl32.lib winmm.lib dbghelp.lib > "%OUT%\link.log" 2>&1
if errorlevel 1 ( type "%OUT%\link.log" & exit /b 1 )
echo built %OUT%\daytona.exe
endlocal
