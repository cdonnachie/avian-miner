# Assemble a zip for someone with an AMD card to run, who should not have to
# know anything about this project.
#
# It reuses the existing Windows release directory, which already holds the
# miner and the MSYS2 runtime DLLs its import table needs, and swaps in the
# HIP-built kernels. The miner itself is not rebuilt: it reaches the backend
# through the plain-data ABI in cuda/avian-cuda-api.h, and the HIP DLL exports
# the same names, so the same binary drives either vendor.
#
# What the tester needs installed: AMD's display driver, which ships
# amdhip64_7.dll. Not the HIP SDK, not this repository, not a compiler.
param(
    [string]$Release = "C:\Users\craig\AppData\Local\Temp\claude\c--development-Avian-v5-0\9e1a4f58-d2d0-4ac5-b59e-ba1e8ee54635\scratchpad\winbuild\release-gpu",
    [string]$HipDll  = "C:\Users\craig\AppData\Local\Temp\claude\c--development-Avian-v5-0\9e1a4f58-d2d0-4ac5-b59e-ba1e8ee54635\scratchpad\winbuild\hip\avian_cuda.dll",
    [string]$Out     = "C:\Users\craig\AppData\Local\Temp\claude\c--development-Avian-v5-0\9e1a4f58-d2d0-4ac5-b59e-ba1e8ee54635\scratchpad\avian-amd-test"
)

foreach ($p in @($Release, $HipDll)) {
    if (-not (Test-Path $p)) { Write-Output "missing: $p"; exit 1 }
}

if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force $Out | Out-Null

# Everything the miner needs except the kernels, then the AMD kernels over
# the top. The Nvidia DLL is deliberately not carried: a tester running this
# on an Nvidia box would otherwise get a pass that says nothing about AMD.
Copy-Item "$Release\*" $Out -Force
Copy-Item $HipDll "$Out\avian_cuda.dll" -Force

$bat = @'
@echo off
rem Runs for one minute, hashing against a fixed test header. Nothing is
rem mined, nothing is sent anywhere, no wallet or pool is involved.
setlocal
cd /d "%~dp0"
echo Running the Avian AMD test for about a minute. Please wait.
echo.
avian-miner.exe -a x16rt --gpu --benchmark > result.txt 2>&1
echo.
echo Done. Please send back the file result.txt from this folder.
echo.
pause
'@
$bat | Set-Content -Encoding ASCII "$Out\RUN-ME-first.bat"

$readme = @'
Avian AMD graphics card test
============================

Thank you for trying this. It takes about a minute and needs no setup.

WHAT IT DOES
  It hashes against a fixed test block to check whether this software
  calculates correctly on AMD graphics cards, and how fast. Nothing is
  mined, nothing is sent over the internet, and no wallet or account is
  involved. It writes one text file and stops.

WHAT YOU NEED
  An AMD graphics card and AMD's normal graphics driver, which you will
  already have if the card works for games. Nothing else to install.

HOW TO RUN IT
  1. Unzip this folder somewhere, for example your Desktop.
  2. Double-click RUN-ME-first.bat
  3. A black window opens for about a minute, then says Done.
  4. Send back the file called result.txt that appears in this folder.

IF IT CLOSES IMMEDIATELY OR SAYS SOMETHING ABOUT A MISSING DLL
  That is useful information too, not a mistake on your part. Please say
  which graphics card you have and send result.txt if there is one.

WHAT WE ARE LOOKING FOR IN result.txt
  Lines saying "did not validate" would mean the card computed something
  wrong, which is exactly what we want to find out. A line saying
  "Total: ... MH/s" tells us the speed. Either way, please send the file.
'@
$readme | Set-Content -Encoding ASCII "$Out\README.txt"

Write-Output "=== package contents ==="
Get-ChildItem $Out | Sort-Object Name | ForEach-Object {
    "  {0,-28} {1,10:N0}" -f $_.Name, $_.Length
}

$zip = "$Out.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$Out\*" -DestinationPath $zip
Write-Output ""
Write-Output "zip: $zip  ($((Get-Item $zip).Length) bytes)"
