@echo off
rem MinotaurX on the CPU, solo over Stratum v2 at sv2-yyz.avn.network.
rem
rem Your Avian address and worker name come from avian-settings.cmd beside
rem this file, which asks for the address the first time and keeps it; the
rem installer fills it in for you. README.md explains everything the miner
rem prints, and webui.md the dashboard.
setlocal
cd /d "%~dp0"
call avian-settings.cmd || exit /b 1

rem The API on 4050 and the dashboard on 4051 rather than the defaults, so
rem this runs beside mine-x16rt.bat on the same machine. The dashboard is
rem readable by anything on your network at http://THIS-PC:4051/ and has no
rem password; drop the --webui line if you do not want it.
rem
rem To run this and the GPU miner together, use mine-both.bat, which gives
rem the GPU miner the last core and this one the rest.
avian-miner.exe -a minotaurx ^
    --sv2=sv2-yyz.avn.network:3333 ^
    --sv2-authority=9bxhNrDTna5QPTXoJebdFBxeAccYNNxva36PhbjAK9EA2moQyjY ^
    --sv2-user=%WALLET%.%WORKER% ^
    --api-bind=127.0.0.1:4050 ^
    --webui=0.0.0.0:4051
pause
