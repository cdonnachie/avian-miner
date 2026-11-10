@echo off
rem Called by the mine-*.bat files beside it: finds your Avian address and a
rem worker name, asking for the address the first time and keeping it.
rem
rem Two places are read, the second overriding the first:
rem   %ProgramData%\avian-miner\settings.cmd   written by the installer
rem   %APPDATA%\avian-miner\settings.cmd       written here, on first run
rem Each is a two-line batch file, set "WALLET=..." and set "WORKER=...",
rem and editing either in Notepad is how to change them later.
rem
rem No setlocal here on purpose: WALLET and WORKER have to reach the caller.
set "AVIAN_MACHINE_CFG=%ProgramData%\avian-miner\settings.cmd"
set "AVIAN_USER_CFG=%APPDATA%\avian-miner\settings.cmd"
set "WALLET="
set "WORKER="
if exist "%AVIAN_MACHINE_CFG%" call "%AVIAN_MACHINE_CFG%"
if exist "%AVIAN_USER_CFG%" call "%AVIAN_USER_CFG%"
if defined WALLET goto have

echo.
echo No Avian address is set yet. Paste the address the rewards should go to
echo and press Enter. It is kept in
echo   %AVIAN_USER_CFG%
echo and can be changed there.
echo.
set /p "WALLET=Avian address: "
if not defined WALLET exit /b 1
if not exist "%APPDATA%\avian-miner" mkdir "%APPDATA%\avian-miner"
> "%AVIAN_USER_CFG%" echo set "WALLET=%WALLET%"
>> "%AVIAN_USER_CFG%" echo rem set "WORKER=myrig"  (the computer name unless set here)
echo Saved.
echo.

:have
if not defined WORKER set "WORKER=%COMPUTERNAME%"
exit /b 0
