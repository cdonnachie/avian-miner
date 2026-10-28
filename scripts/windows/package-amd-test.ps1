# Assemble a zip for someone with an AMD card to run, who should not have to
# know anything about this project.
#
# It reuses the existing Windows release directory, which already holds the
# miner and the MSYS2 runtime DLLs its import table needs, and swaps in the
# HIP-built kernels. The miner itself is not rebuilt: it reaches the backend
# through the plain-data ABI in cuda/avian-cuda-api.h, and the HIP DLL exports
# the same names, so the same binary drives either vendor.
#
# What the tester needs installed: AMD's display driver, and nothing
# else. Not the HIP SDK, not this repository, not a compiler. The HIP
# runtime is not a driver file and travels in the zip.
param(
    # The plain release directory, not release-gpu. That one is a hand-made
    # copy kept for swapping DLLs by hand, so its miner is whatever was
    # last dropped into it, and right now it has no miner at all. This
    # one is written by the build and carries the corrected banner.
    [string]$Release = "C:\Users\craig\AppData\Local\Temp\claude\c--development-Avian-v5-0\9e1a4f58-d2d0-4ac5-b59e-ba1e8ee54635\scratchpad\winbuild\release",
    [string]$HipDll  = "C:\Users\craig\AppData\Local\Temp\claude\c--development-Avian-v5-0\9e1a4f58-d2d0-4ac5-b59e-ba1e8ee54635\scratchpad\winbuild\hip\avian_cuda.dll",
    [string]$Out     = "C:\Users\craig\AppData\Local\Temp\claude\c--development-Avian-v5-0\9e1a4f58-d2d0-4ac5-b59e-ba1e8ee54635\scratchpad\avian-amd-test",
    [int]$Seconds    = 60,
    # Leaves the HIP runtime out, so the import resolves to the copy the
    # driver installed. Right for a 6.x build, where consumer drivers
    # supply amdhip64_6.dll and ours would drag in 121 MB of comgr.
    # Wrong for a 7.x build, where no consumer driver has the file.
    [switch]$NoRuntime,
    [string]$Rocm    = "C:\Program Files\AMD\ROCm\7.1\7.1"
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

# Checked rather than assumed. Defender flags this miner
# Trojan:Win32/Bearfoos.B!ml, an ml-suffixed machine-learning guess
# rather than a signature match, and deletes it from the build directory
# without the copy above reporting anything. Twice today that produced a
# 27-file zip with no miner in it, which is indistinguishable to a tester
# from the package simply not working.
if (-not (Test-Path "$Out\avian-miner.exe")) {
    Write-Output ""
    Write-Output "avian-miner.exe did not reach the package."
    Write-Output "Source: $Release"
    if (-not (Test-Path "$Release\avian-miner.exe")) {
        Write-Output "It is not in the source directory either, so it has been removed"
        Write-Output "since the build. Check Get-MpThreatDetection: this miner is flagged"
        Write-Output "Trojan:Win32/Bearfoos.B!ml and needs a Defender exclusion for the"
        Write-Output "build directory, or restoring from quarantine, both of which need"
        Write-Output "an elevated shell."
    }
    # The stale zip goes too. A previous good one sitting next to a failed
    # run is worse than nothing: it is the file that would get sent.
    Remove-Item -Recurse -Force $Out
    if (Test-Path "$Out.zip") { Remove-Item -Force "$Out.zip" }
    exit 1
}

# The HIP runtime travels with the package. It is not a driver file:
# amdhip64_7.dll is an SDK redistributable, and a machine carrying only the
# Adrenalin driver does not have it. Leaving it out made the first tester's
# run produce a result.txt with nothing in it but the heading, because
# Windows creates the process, fails to resolve the import, and kills it
# before a single line reaches stdout.
#
# amd_comgr is deliberately not shipped. It is 114 MB and wanted only for
# compiling kernels at run time, which this does not do.
# Found by pattern rather than named. The runtime carries only its major
# version in the filename, so 7.1.1 and 7.2.0 both produce amdhip64_7.dll:
# interchangeable by name and not by content, and 7.2.0 dropped the RDNA2
# cards that 7.1.1 still supports. Naming the file outright would ship
# whichever SDK happened to be looked at, which is how a 6900 XT ends up
# resolving the import cleanly and then being declined.
$hipRt = Get-ChildItem (Join-Path $Rocm "bin") -Filter "amdhip64_*.dll" -ErrorAction SilentlyContinue |
         Select-Object -First 1
if (-not $hipRt) { Write-Output "no amdhip64_*.dll under $Rocm\bin"; exit 1 }
if ($NoRuntime) {
    Write-Output "  HIP runtime: not shipped, $($hipRt.Name) must come from the driver"
} else {
    Copy-Item $hipRt.FullName $Out -Force
    Write-Output "  HIP runtime: $($hipRt.Name), from $Rocm"
    # Whatever the runtime statically imports has to come too. 6.4's
    # names amd_comgr_2.dll, which is 121 MB; 7.1.1's imports nothing
    # AMD-specific and loads comgr only to compile at run time, which
    # this does not do. Shipping the one without the other gets a
    # process that dies at load having printed nothing.
    $comgr = Join-Path $Rocm "bin\amd_comgr_2.dll"
    if (Test-Path $comgr) {
        Write-Output "  note: $($hipRt.Name) imports amd_comgr_2.dll, $([int]((Get-Item $comgr).Length / 1MB)) MB"
        Write-Output "  shipping it, which is why this zip is large. -NoRuntime avoids both."
        Copy-Item $comgr $Out -Force
    }
}

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
    # The miner first, because antivirus removing it is the single most
    # likely reason this folder does not work, and Start-Process on a
    # missing file would otherwise leave result.txt as a bare heading.
    `$exe = Join-Path `$PSScriptRoot 'avian-miner.exe'
    if (-not (Test-Path `$exe)) {
        Add-Content -Path `$res -Value "==== `$label ===="
        Add-Content -Path `$res -Value 'avian-miner.exe is missing from this folder.'
        Add-Content -Path `$res -Value 'Antivirus has almost certainly removed it. See README.txt.'
        Write-Host 'avian-miner.exe is missing. Your antivirus has very likely'
        Write-Host 'removed it. Please read README.txt and send back result.txt.'
        return
    }
    `$o = Join-Path `$PSScriptRoot 'bench-out.txt'
    `$e = Join-Path `$PSScriptRoot 'bench-err.txt'
    `$p = Start-Process -FilePath `$exe ``
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

# The heading says which runtime was used, because result.txt is read
# later without the command that produced it to hand.
Run-Once '$(if ($NoRuntime) { "benchmark run, using the runtime from the driver" } else { "benchmark run, with the runtime from this folder" })' @{}

# A second attempt only if the first found nothing, and one that is
# genuinely different: the runtime we shipped is moved aside so the
# import resolves to the copy the driver installed in system32.
#
# The previous retry here set HSA_OVERRIDE_GFX_VERSION, which Windows
# HIP never reads, so it was the same run twice and two testers duly
# sent back files whose halves were identical. This one can only differ
# or fail to start, and the runner reports either.
if (Select-String -Path `$res -Pattern 'no CUDA device found' -Quiet) {
    `$rt = '$($hipRt.Name)'
    `$shipped = Join-Path `$PSScriptRoot `$rt
    if (Test-Path `$shipped) {
        Write-Host 'No card found. Trying once more with your own AMD'
        Write-Host 'runtime instead of the one in this folder.'
        Write-Host ''
        Rename-Item `$shipped "`$rt.unused"
        Run-Once "second run, using the driver's own `$rt" @{}
        Rename-Item (Join-Path `$PSScriptRoot "`$rt.unused") `$rt
    }
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
  It will try once more, a different way, which takes another minute.
  Please send result.txt either way. That is a real result and not a
  failure on your part.

YOUR ANTIVIRUS MAY OBJECT, OR QUIETLY DELETE avian-miner.exe
  Please expect this. Mining software gets flagged as a matter of course,
  because it is built from the same kind of code as the mining programs
  that get installed on people's machines without asking. Microsoft
  Defender currently calls this one Trojan:Win32/Bearfoos.B!ml. The "ml"
  on the end means its machine-learning scanner guessed from the shape of
  the file; it is not a signature match for any actual known malware.

  It is your machine and your call. If you would rather not make an
  exception for a program a friend sent you, that is a completely
  reasonable answer and we would rather you said so than went ahead
  uncomfortable. If the folder loses avian-miner.exe, just tell us, and
  send result.txt if there is one: that outcome is itself useful to know.

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

# -ErrorAction Stop, because Compress-Archive reports a file it could
# not read as a non-terminating error and then carries on to write a
# zip missing that file, or no zip at all while the script prints a
# success line for it.
try {
    Compress-Archive -Path "$Out\*" -DestinationPath $zip -ErrorAction Stop
} catch {
    Write-Output ""
    Write-Output "could not build the zip: $($_.Exception.Message)"
    if ($_.Exception.Message -match "virus|unwanted") {
        Write-Output ""
        Write-Output "Defender is blocking the read. Restoring the miner from quarantine"
        Write-Output "is not enough on its own: without an exclusion it stays blocked and"
        Write-Output "will be deleted again. In an elevated shell:"
        Write-Output "  Add-MpPreference -ExclusionPath <the build directory>"
    }
    if (Test-Path $zip) { Remove-Item -Force $zip }
    exit 1
}

# The zip is opened and read back, rather than trusted because the
# command returned. This is the only check on the file that actually
# gets sent to someone, and every earlier guard here was added after
# a package shipped or nearly shipped incomplete.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$want = @("avian-miner.exe", "avian_cuda.dll",
          "run-test.ps1", "RUN-ME-first.bat", "README.txt")
if (-not $NoRuntime) { $want += $hipRt.Name }
$have = @()
$a = [IO.Compression.ZipFile]::OpenRead($zip)
try { $have = $a.Entries | ForEach-Object { $_.Name } }
finally { $a.Dispose() }
$missing = $want | Where-Object { $have -notcontains $_ }
if ($missing) {
    Write-Output ""
    Write-Output "the zip is missing: $($missing -join ', ')"
    Remove-Item -Force $zip
    exit 1
}

Write-Output ""
Write-Output "zip: $zip"
Write-Output "  $((Get-Item $zip).Length) bytes, $($have.Count) files, all six expected present"
