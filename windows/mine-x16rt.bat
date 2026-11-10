@echo off
rem X16RT on the GPU, solo over Stratum v2 at sv2-yyz.avn.network.
rem
rem Your Avian address and worker name come from avian-settings.cmd beside
rem this file, which asks for the address the first time and keeps it; the
rem installer fills it in for you. README.md explains everything the miner
rem prints, and webui.md the dashboard.
setlocal
cd /d "%~dp0"
call avian-settings.cmd || exit /b 1

rem Every card the miner finds, Nvidia and AMD alike; --devices 0,2 picks a
rem subset, numbered as --list-devices prints them. The dashboard is
rem readable by anything on your network at http://THIS-PC:4049/ and has no
rem password; drop the --webui line if you do not want it.
rem
rem To run this and the CPU miner together, use mine-both.bat, which gives
rem this one the last core and the CPU miner the rest.
avian-miner.exe -a x16rt --gpu ^
    --sv2=sv2-yyz.avn.network:3333 ^
    --sv2-authority=9bxhNrDTna5QPTXoJebdFBxeAccYNNxva36PhbjAK9EA2moQyjY ^
    --sv2-user=%WALLET%.%WORKER% ^
    --webui
pause
