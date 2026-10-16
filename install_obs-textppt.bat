@echo off
setlocal EnableExtensions DisableDelayedExpansion
chcp 65001 >nul
title OBS-TextPPT Windows Installer

echo OBS-TextPPT 5.0.0 - Windows x64 / OBS 32
echo.
if not exist "%~dp0obs-textppt\bin\64bit\obs-textppt.dll" (
  echo ERROR: Extract the complete ZIP before running this script.
  goto failed
)

fltmc >nul 2>&1
if errorlevel 1 (
  echo Right-click install_obs-textppt.bat and select "Run as administrator".
  goto failed
)

tasklist /FI "IMAGENAME eq obs64.exe" /NH 2>nul | find /I "obs64.exe" >nul
if not errorlevel 1 (
  echo Close all OBS windows before installing.
  goto failed
)

set "OBS_TEXTPPT_TARGET=%ProgramData%\obs-studio\plugins\obs-textppt"
for %%I in ("%~dp0obs-textppt") do set "OBS_TEXTPPT_SOURCE=%%~fI"
for %%I in ("%OBS_TEXTPPT_TARGET%") do set "OBS_TEXTPPT_TARGET=%%~fI"
set "OBS_TEXTPPT_CHECK=%OBS_TEXTPPT_SOURCE%"
:check_path
if /I "%OBS_TEXTPPT_CHECK%"=="%OBS_TEXTPPT_TARGET%" (
  echo ERROR: Extract the package outside the installation folder.
  goto failed
)
for %%I in ("%OBS_TEXTPPT_CHECK%\..") do set "OBS_TEXTPPT_PARENT=%%~fI"
if /I "%OBS_TEXTPPT_CHECK%"=="%OBS_TEXTPPT_PARENT%" goto paths_checked
set "OBS_TEXTPPT_CHECK=%OBS_TEXTPPT_PARENT%"
goto check_path

:paths_checked
for %%D in ("%ProgramData%" "%ProgramData%\obs-studio" "%ProgramData%\obs-studio\plugins" "%OBS_TEXTPPT_TARGET%") do (
  if exist "%%~D" (
    fsutil reparsepoint query "%%~D" >nul 2>&1
    if not errorlevel 1 (
      echo ERROR: The installation path must not contain directory links.
      goto failed
    )
  )
)
for %%L in (en-US zh-CN zh-TW) do (
  if not exist "%OBS_TEXTPPT_SOURCE%\data\locale\%%L.ini" (
    echo ERROR: Language files are missing. Extract the complete ZIP.
    goto failed
  )
)
echo Installing to:
echo "%OBS_TEXTPPT_TARGET%"
echo.
if not exist "%OBS_TEXTPPT_TARGET%" mkdir "%OBS_TEXTPPT_TARGET%"
if not exist "%OBS_TEXTPPT_TARGET%" (
  echo ERROR: Cannot create the installation folder.
  goto failed
)

rem Mirror only this plugin directory; obsolete plugin files are removed.
"%SystemRoot%\System32\robocopy.exe" "%OBS_TEXTPPT_SOURCE%" "%OBS_TEXTPPT_TARGET%" /MIR /COPY:DAT /DCOPY:DAT /R:1 /W:1 /XJ /NP /NFL /NDL /NJH /NJS
if errorlevel 8 (
  echo ERROR: Synchronization failed. Check permissions and close OBS.
  goto failed
)

echo Installed successfully. Restart OBS and add a TextPPT source.
echo Open the TextPPT dock from the Docks menu.
set "OBS_TEXTPPT_RESULT=0"
goto finish

:failed
set "OBS_TEXTPPT_RESULT=1"

:finish
echo.
rem Close after five seconds or any console key, preserving the install result.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -Command "$interactive = -not [Console]::IsInputRedirected; $previous = $false; if ($interactive) { $previous = [Console]::TreatControlCAsInput; [Console]::TreatControlCAsInput = $true }; try { $clock = [Diagnostics.Stopwatch]::StartNew(); $last = 0; while ($clock.Elapsed.TotalSeconds -lt 5) { $seconds = 5 - [int][Math]::Floor($clock.Elapsed.TotalSeconds); if ($seconds -ne $last) { [Console]::Write([string][char]13 + ('Closing in {0}s... Press any key to exit. ' -f $seconds)); $last = $seconds }; if ($interactive -and [Console]::KeyAvailable) { $null = [Console]::ReadKey($true); break }; Start-Sleep -Milliseconds 50 } } finally { if ($interactive) { [Console]::TreatControlCAsInput = $previous }; [Console]::WriteLine() }"
exit /b %OBS_TEXTPPT_RESULT%
