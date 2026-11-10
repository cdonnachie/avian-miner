@echo off
rem X16RT on the GPU, solo over Stratum v2 at sv2-yyz.avn.network.
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

rem Every card the miner finds, Nvidia and AMD alike; --devices 0,2 picks a
rem subset, numbered as --list-devices prints them. The dashboard is
rem readable by anything on your network at http://THIS-PC:4049/ and has no
rem password; drop the --webui line if you do not want it.
rem
rem To run this and the CPU miner together, use mine-both.bat, which gives
rem this one the last core and the CPU miner the rest.
cd /d "%~dp0"
avian-miner.exe -a x16rt --gpu ^
    --sv2=sv2-yyz.avn.network:3333 ^
    --sv2-authority=9bxhNrDTna5QPTXoJebdFBxeAccYNNxva36PhbjAK9EA2moQyjY ^
    --sv2-user=%WALLET%.%WORKER% ^
    --webui
pause
