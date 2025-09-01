@echo off
setlocal
REM Always run from this folder
cd /d "%~dp0" || exit /b 1

REM Prefer PS7, fallback to Windows PowerShell
set "PWSH=%ProgramFiles%\PowerShell\7\pwsh.exe"
if not exist "%PWSH%" set "PWSH=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"

"%PWSH%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_shaders.ps1" -SrcDir "%~dp0" -OutDir "%~dp0"
set ERR=%ERRORLEVEL%
if %ERR% neq 0 (
  echo build_shaders.ps1 failed with exit code %ERR%
  exit /b %ERR%
)
echo Shaders built OK.
exit /b 0