@echo off
rem MinotaurX on the CPU and X16RT on the GPU at once, each in its own
rem window, solo over Stratum v2 at sv2-yyz.avn.network.
rem
rem Your Avian address and worker name come from avian-settings.cmd beside
rem this file, which asks for the address the first time and keeps it; the
rem installer fills it in for you.
rem
rem The two share the CPU like this: the GPU miner's one thread gets the
rem last core, both of its hyperthreads, so a card is never waiting on a
rem core that is busy hashing; MinotaurX gets every other CPU, one thread
rem each. On a 24-thread CPU that is 0-21 and 22-23, on a 16-thread one
rem 0-13 and 14-15. It assumes the last two CPU numbers are one core's two
rem threads, which is how Windows numbers every Intel and AMD desktop CPU;
rem on a CPU without hyperthreading it hands the GPU miner two cores, which
rem is only generous. avian-miner --list-cpus prints this machine's layout
rem and the values it would choose, for checking or for a different split.
setlocal
cd /d "%~dp0"
call avian-settings.cmd || exit /b 1

set /a N=%NUMBER_OF_PROCESSORS%
if %N% LSS 4 (
    echo With %N% CPUs there is nothing to split. Run mine-x16rt.bat or
    echo mine-minotaurx.bat on its own.
    pause
    exit /b 1
)
set /a CPU_LAST=N-3
set /a GPU_FIRST=N-2
set /a GPU_LAST=N-1

rem Two windows, each kept open when its miner stops so the reason stays
rem on screen. The GPU miner has the default ports, 4048 and 4049; the CPU
rem one takes 4050 and 4051. Both dashboards are readable by anything on
rem your network and have no password.
start "avian-miner x16rt (GPU, CPUs %GPU_FIRST%-%GPU_LAST%)" cmd /k avian-miner.exe -a x16rt --gpu ^
    --cpu-affinity %GPU_FIRST%-%GPU_LAST% ^
    --sv2=sv2-yyz.avn.network:3333 ^
    --sv2-authority=9bxhNrDTna5QPTXoJebdFBxeAccYNNxva36PhbjAK9EA2moQyjY ^
    --sv2-user=%WALLET%.%WORKER%-gpu ^
    --webui
start "avian-miner minotaurx (CPU, CPUs 0-%CPU_LAST%)" cmd /k avian-miner.exe -a minotaurx ^
    --cpu-affinity 0-%CPU_LAST% ^
    --sv2=sv2-yyz.avn.network:3333 ^
    --sv2-authority=9bxhNrDTna5QPTXoJebdFBxeAccYNNxva36PhbjAK9EA2moQyjY ^
    --sv2-user=%WALLET%.%WORKER%-cpu ^
    --api-bind=127.0.0.1:4050 ^
    --webui=0.0.0.0:4051
