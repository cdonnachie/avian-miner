@echo off
rem MinotaurX on the CPU, solo over Stratum v2 at sv2-yyz.avn.network.
rem
rem Put your Avian address in the WALLET line and this file is ready to run.
rem WORKER is how this machine is named on the pool; the computer name is a
rem fine default. README.md beside this file explains everything the miner
rem prints, and webui.md the dashboard.
setlocal

set WALLET=YOUR_AVN_ADDRESS
set WORKER=%COMPUTERNAME%

if "%WALLET%"=="YOUR_AVN_ADDRESS" (
    echo Open %~nx0 in Notepad and put your Avian address in the WALLET line.
    pause
    exit /b 1
)

rem The API on 4050 and the dashboard on 4051 rather than the defaults, so
rem this runs beside mine-x16rt.bat on the same machine. The dashboard is
rem readable by anything on your network at http://THIS-PC:4051/ and has no
rem password; drop the --webui line if you do not want it.
rem
rem To run this and the GPU miner together, use mine-both.bat, which gives
rem the GPU miner the last core and this one the rest.
cd /d "%~dp0"
avian-miner.exe -a minotaurx ^
    --sv2=sv2-yyz.avn.network:3333 ^
    --sv2-authority=9bxhNrDTna5QPTXoJebdFBxeAccYNNxva36PhbjAK9EA2moQyjY ^
    --sv2-user=%WALLET%.%WORKER% ^
    --api-bind=127.0.0.1:4050 ^
    --webui=0.0.0.0:4051
pause
