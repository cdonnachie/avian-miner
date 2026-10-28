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
    [string]$Out     = "C:\Users\craig\AppData\Local\Temp\claude\c--development-Avian-v5-0\9e1a4f58-d2d0-4ac5-b59e-ba1e8ee54635\scratchpad\avian-amd-test",
    [int]$Seconds    = 60,
    [string]$Rocm    = "C:\Program Files\AMD\ROCm\7.2"
)

foreach ($p in @($Release, $HipDll)) {
    if (-not (Test-Path $p)) { Write-Output "missing: $p"; exit 1 }
}

if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force $Out | Out-Null

# The executable and its DLLs, and nothing else. Copying the directory
# wholesale once carried a result.txt from a previous run into the package,
# which is the one filename the tester is told to send back: they would have
# had someone else's output sitting there before they started.
#
# The Nvidia kernels are then overwritten by the AMD ones. Not merely
# overwritten but deliberately not shipped: a tester with both vendors in the
# machine would otherwise get a clean pass that says nothing about AMD.
Copy-Item "$Release\*.exe" $Out -Force
Copy-Item "$Release\*.dll" $Out -Force
Copy-Item $HipDll "$Out\avian_cuda.dll" -Force

# The HIP runtime travels with the package. It is not a driver file:
# amdhip64_7.dll is an SDK redistributable, and a machine carrying only the
# Adrenalin driver does not have it. Leaving it out made the first tester's
# run produce a result.txt with nothing in it but the heading, because
# Windows creates the process, fails to resolve the import, and kills it
# before a single line reaches stdout.
#
# amd_comgr is deliberately not shipped. It is 114 MB and wanted only for
# compiling kernels at run time, which this does not do.
$hipRt = Join-Path $Rocm "bin\amdhip64_7.dll"
if (-not (Test-Path $hipRt)) { Write-Output "missing HIP runtime: $hipRt"; exit 1 }
Copy-Item $hipRt $Out -Force

# The work is in PowerShell rather than the batch file, because the batch
# file had no way to stop the miner. Windows `timeout` sleeps, it does not
# end a process, so the first version of this ran until the tester closed the
# window while the instructions said it would take a minute.
$runner = @"
# Runs the miner for a fixed time and stops it. Started with -PassThru and
# killed by handle, not by name: taskkill /im would also end any other
# avian-miner on the machine, which on a miner's own computer is the one
# earning them money.
`$ErrorActionPreference = 'SilentlyContinue'
Set-Location -LiteralPath `$PSScriptRoot

function Run-Once([string]`$label, [hashtable]`$extraEnv) {
    foreach (`$k in `$extraEnv.Keys) { Set-Item -Path "env:`$k" -Value `$extraEnv[`$k] }
    `$o = Join-Path `$PSScriptRoot 'bench-out.txt'
    `$e = Join-Path `$PSScriptRoot 'bench-err.txt'
    `$p = Start-Process -FilePath (Join-Path `$PSScriptRoot 'avian-miner.exe') ``
            -ArgumentList '-a','x16rt','--gpu','--benchmark' ``
            -RedirectStandardOutput `$o -RedirectStandardError `$e ``
            -NoNewWindow -PassThru
    # Touching Handle caches it, without which ExitCode comes back empty
    # after the process has gone, which is exactly when it is wanted.
    `$null = `$p.Handle
    `$stopped = `$false
    if (-not `$p.WaitForExit($($Seconds * 1000))) {
        `$p.Kill()
        [void]`$p.WaitForExit(5000)
        `$stopped = `$true
    }
    Add-Content -Path `$res -Value "==== `$label ===="
    # The exit code, always. A dependent DLL that cannot be resolved makes
    # Windows create the process and end it immediately with 0xC0000135,
    # printing nothing at all, and the first tester's result.txt was a
    # heading and silence. The code says what the silence meant.
    if (-not `$stopped) {
        `$c = `$p.ExitCode
        Add-Content -Path `$res -Value ("exited on its own, code " + `$c +
            `$(if (`$c -eq -1073741515) { "  (0xC0000135, a required DLL was not found)" }
              elseif (`$c -eq -1073741701) { "  (0xC000007B, a DLL is the wrong architecture)" }
              else { "" }))
    }
    foreach (`$f in @(`$o, `$e)) {
        if (Test-Path `$f) { Get-Content `$f | Add-Content -Path `$res }
    }
    if (-not (Get-Content `$res | Where-Object { `$_ -match 'avian-miner|GPU #|Total:' })) {
        Add-Content -Path `$res -Value "the miner produced no output whatsoever"
    }
    Remove-Item `$o, `$e -Force -ErrorAction SilentlyContinue
    foreach (`$k in `$extraEnv.Keys) { Remove-Item -Path "env:`$k" -ErrorAction SilentlyContinue }
}

`$res = Join-Path `$PSScriptRoot 'result.txt'
Remove-Item `$res -Force -ErrorAction SilentlyContinue

Write-Host ''
Write-Host 'Running the Avian AMD test. This takes about a minute.'
Write-Host ''
Write-Host 'Lines saying "failed" are expected: the test is not connected to'
Write-Host 'anything, so it cannot send its results anywhere. Please ignore them.'
Write-Host ''

Run-Once 'first attempt' @{}

# AMD's runtime declines some of the smaller RDNA2 cards by name, the RX 6600
# among them. Telling it to treat the card as an RX 6800 is the usual way
# round that, and it works because gfx1030 is in the same DLL.
if (Select-String -Path `$res -Pattern 'no CUDA device found' -Quiet) {
    Write-Host 'No card found. Trying once more with a compatibility setting.'
    Write-Host ''
    Run-Once 'second attempt, HSA_OVERRIDE_GFX_VERSION=10.3.0' @{ HSA_OVERRIDE_GFX_VERSION = '10.3.0' }
}

Write-Host ''
Write-Host 'Done. Please send back the file result.txt from this folder.'
Write-Host ''
"@
$runner | Set-Content -Encoding ASCII "$Out\run-test.ps1"

$bat = @'
@echo off
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-test.ps1"
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
  involved. It writes one text file and stops on its own.

WHAT YOU NEED
  An AMD graphics card and AMD's normal graphics driver, which you will
  already have if the card works for games. Nothing else to install.

HOW TO RUN IT
  1. Unzip this folder somewhere, for example your Desktop.
  2. Double-click RUN-ME-first.bat
  3. A black window opens for about a minute, then says Done.
  4. Send back the file called result.txt that appears in this folder.

YOU WILL SEE SOME LINES SAYING "failed". THAT IS NORMAL.
  Specifically "HTTP request failed: No URL set" and
  "submit_upstream_work json_rpc_call failed", over and over. The test
  deliberately uses a target so easy that it "finds" a result every
  second, and since it is not connected to anything, sending them off
  fails every time. Nothing is wrong and nothing is being sent anywhere.
  Please ignore those and send the file regardless.

IF IT SAYS IT CANNOT FIND YOUR CARD
  It will automatically try a second time with a compatibility setting,
  which is normal and takes another minute. Either way, send result.txt.

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
