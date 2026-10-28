# Assemble a small zip that answers one question: does this machine's AMD
# card get seen, and by what.
#
# This exists because a tester's 6900 XT reported "no ROCm-capable device is
# detected" and there was no way to tell, from the miner's output alone,
# whether the driver could not see the card, the HIP runtime was declining
# it, or our own kernels were at fault. Three very different problems with
# one symptom.
#
# It ships AMD's own tools rather than ours, which is the point: if AMD's
# probe cannot find the card either, nothing in this repository is involved.
#
#   amdgpu-arch.exe  asks the driver directly. Its imports are ntdll,
#                    kernel32, shell32 and ole32 and nothing AMD-specific,
#                    so it works with no HIP runtime present at all. It
#                    prints the gfx name, which is the one fact that decides
#                    whether a card is in our build's architecture list.
#
#   hipInfo.exe      asks the HIP runtime, needing only amdhip64_7.dll. So a
#                    card that amdgpu-arch finds and hipInfo does not is the
#                    runtime refusing supported hardware, which is a version
#                    choice on our side, not a fault on theirs.
#
# No kernels, no miner. Nothing here is flagged by antivirus, which also
# makes it a clean thing to send someone whose Defender ate the last zip.
param(
    [string]$Out  = $(if ($env:AVIAN_BUILD_DIR) { "$env:AVIAN_BUILD_DIR\avian-amd-probe" } else { Join-Path (Resolve-Path "$PSScriptRoot\..\..\..") "avian-winbuild\avian-amd-probe" }),
    [string]$Rocm = "C:\Program Files\AMD\ROCm\7.1\7.1"
)

$bin = Join-Path $Rocm "bin"
$need = @("amdgpu-arch.exe", "hipInfo.exe")
foreach ($f in $need) {
    if (-not (Test-Path (Join-Path $bin $f))) {
        Write-Output "missing: $bin\$f"; exit 1
    }
}

# A failure to clear this is fatal rather than ignored. It was not, and a
# rebuild whose Remove-Item lost to an open handle carried on copying
# over whatever was already there, which is how a stale file ends up in
# a package that then verifies clean on filenames alone.
if (Test-Path $Out) {
    Remove-Item -Recurse -Force $Out -ErrorAction SilentlyContinue
    if (Test-Path $Out) {
        Write-Output "could not clear $Out, something has it open"
        exit 1
    }
}
New-Item -ItemType Directory -Force $Out | Out-Null

foreach ($f in $need) { Copy-Item (Join-Path $bin $f) $Out -Force }

# Found by pattern for the same reason as in package-amd-test.ps1: both SDK
# versions call it amdhip64_7.dll, so the name alone does not say which one
# this is, and which one it is was the whole question last time.
$hipRt = Get-ChildItem $bin -Filter "amdhip64_*.dll" -ErrorAction SilentlyContinue |
         Select-Object -First 1
if (-not $hipRt) { Write-Output "no amdhip64_*.dll under $bin"; exit 1 }
Copy-Item $hipRt.FullName $Out -Force

# A single-quoted here-string, so nothing inside needs escaping. The test
# package's runner uses a double-quoted one and every $ and backtick in it
# has to be escaped by hand, which is a steady source of mistakes for no
# gain once there is nothing to interpolate.
$probe = @'
$ErrorActionPreference = 'SilentlyContinue'
Set-Location -LiteralPath $PSScriptRoot
$out = Join-Path $PSScriptRoot 'probe.txt'
Remove-Item $out -Force -ErrorAction SilentlyContinue

function Say([string]$s) { Add-Content -Path $out -Value $s }

Write-Host ''
Write-Host 'Checking what this machine reports about its AMD card.'
Write-Host 'This takes a few seconds and changes nothing on your computer.'
Write-Host ''

Say "probe taken $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') local time"
Say "windows $([Environment]::OSVersion.Version.ToString())"
Say ''

# The driver version is the thing most likely to explain a card that the
# runtime will not touch, and it is not something a tester can be expected
# to read off by hand.
Say '==== display adapters, as Windows sees them ===='
Get-CimInstance Win32_VideoController | ForEach-Object {
    Say "  name            : $($_.Name)"
    Say "  driver version  : $($_.DriverVersion)"
    Say "  driver date     : $($_.DriverDate)"
    Say "  status          : $($_.Status)"
    Say ''
}

# Through cmd, because both tools say why they failed on stderr and calling
# them directly loses it: PowerShell turns a native command's stderr into
# error records, which SilentlyContinue above then discards. The first run of
# this printed "(no output)" where the tool had actually said "Failed to get
# device count", which is the most informative line either tool produces
# when there is no card.
function Probe([string]$exeName, [string]$heading) {
    Say "==== $heading ===="
    $exe = Join-Path $PSScriptRoot $exeName
    $lines = cmd /c "`"$exe`" 2>&1"
    Say "  exit code: $LASTEXITCODE"
    if ($lines) { $lines | ForEach-Object { Say "  $_" } }
    else { Say '  (the tool printed nothing at all)' }
    Say ''
}

Probe 'amdgpu-arch.exe' 'amdgpu-arch: does the driver report an AMD GPU, and which'
Probe 'hipInfo.exe' 'hipInfo: does the HIP runtime see it'

# First, because it is the one answer that means nothing is wrong. AMD's
# compute runtime cannot enumerate a GPU in a Remote Desktop session: the
# session is handed a different display adapter, and HIP reports no device
# on hardware that works perfectly from the console.
Say '==== is this a local session or a remote one ===='
Say "  SESSIONNAME     : $(if ($env:SESSIONNAME) { $env:SESSIONNAME } else { '(not set)' })"
Say "  session id      : $((Get-Process -Id $PID).SessionId)"
$remote = ($env:SESSIONNAME -and $env:SESSIONNAME -ne 'Console')
Say "  looks remote    : $remote"
if ($remote) {
    Say '  NOTE: a remote session is reason enough on its own for no compute'
    Say '  device to be found. Worth rerunning from the machine itself.'
}
Say ''

# Which AMD runtime pieces the driver installed, and crucially what version
# the driver's own HIP runtime is. system32 holding amdhip64_6.dll and no
# amdhip64_7.dll says the compute stack is 6.x, which decides whether a 6.x
# build of the kernels is worth making.
Say '==== AMD runtime components in system32 ===='
foreach ($d in @('amdhip64.dll', 'amdhip64_6.dll', 'amdhip64_7.dll',
                 'amd_comgr.dll', 'amdihk64.dll', 'amdxc64.dll',
                 'OpenCL.dll', 'amdocl64.dll')) {
    $p = Join-Path $env:SystemRoot "System32\$d"
    if (Test-Path $p) {
        $i = (Get-Item $p).VersionInfo
        Say ("  {0,-16} present, {1,12:N0} bytes, version {2}" -f
             $d, (Get-Item $p).Length, $(if ($i.FileVersion) { $i.FileVersion } else { 'none' }))
    } else { Say ("  {0,-16} absent" -f $d) }
}
Say ''

# Absence from system32 is not absence. Modern AMD drivers keep the compute
# libraries in DriverStore and point at them through the ICD registry, so
# the two that looked missing last time are very likely here instead.
Say '==== the same libraries in DriverStore ===='
$ds = Join-Path $env:SystemRoot 'System32\DriverStore\FileRepository'
foreach ($d in @('amdhip64.dll', 'amdocl64.dll', 'amdihk64.dll')) {
    $hits = Get-ChildItem $ds -Filter $d -Recurse -ErrorAction SilentlyContinue |
            Select-Object -First 2
    if ($hits) { $hits | ForEach-Object { Say "  $d  ->  $($_.FullName)" } }
    else { Say "  $d  not found in DriverStore either" }
}
Say ''

Say '==== OpenCL vendors registered ===='
foreach ($k in @('HKLM:\SOFTWARE\Khronos\OpenCL\Vendors',
                 'HKLM:\SOFTWARE\WOW6432Node\Khronos\OpenCL\Vendors')) {
    Say "  $k"
    $p = Get-Item $k -ErrorAction SilentlyContinue
    if (-not $p) { Say '    (key does not exist)'; continue }
    $names = $p.GetValueNames()
    if (-not $names) { Say '    (no vendors registered)' }
    foreach ($n in $names) { Say "    $n" }
}
Say ''

# Win32_VideoController reports OK whatever the PnP problem code is, so the
# device's own status is asked for separately.
Say '==== the device, as PnP sees it ===='
Get-PnpDevice -Class Display -ErrorAction SilentlyContinue | ForEach-Object {
    Say "  $($_.FriendlyName)"
    Say "    status $($_.Status), problem $($_.Problem)"
}

Write-Host 'Done. Please send back the file probe.txt from this folder.'
Write-Host ''
'@
$probe | Set-Content -Encoding ASCII "$Out\probe.ps1"

$bat = @'
@echo off
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0probe.ps1"
pause
'@
$bat | Set-Content -Encoding ASCII "$Out\RUN-ME-first.bat"

$readme = @'
Avian AMD graphics card probe
=============================

This is a short check, not the miner. It takes a few seconds, mines
nothing, sends nothing over the internet, installs nothing, and changes
nothing on your computer. It writes one text file and stops.

WHAT IT IS FOR
  The last test could not find your graphics card for computing, even
  though the card itself is working fine. We have since established
  that AMD's own tools cannot find it either, so this is not a problem
  with our software. This checks the handful of things that would
  explain that. Two of the three programs in this folder are AMD's own.

ONE THING WORTH SAYING UP FRONT
  If you ran the last test over Remote Desktop, or any other way of
  connecting to that computer from somewhere else, that alone explains
  it and nothing is wrong. Graphics cards are not available for this
  kind of work in a remote session. If so, please run this one sitting
  at the machine itself.

HOW TO RUN IT
  1. Unzip this folder somewhere, for example your Desktop.
  2. Double-click RUN-ME-first.bat
  3. Send back the file called probe.txt that appears in this folder.

Lines in probe.txt saying "absent" or "Failed" are useful answers and
not signs that you did anything wrong. Please send the file either way.
'@
$readme | Set-Content -Encoding ASCII "$Out\README.txt"

Write-Output "=== probe contents ==="
Get-ChildItem $Out | Sort-Object Name | ForEach-Object {
    "  {0,-22} {1,10:N0}" -f $_.Name, $_.Length
}

$zip = "$Out.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
try {
    Compress-Archive -Path "$Out\*" -DestinationPath $zip -ErrorAction Stop
} catch {
    Write-Output "could not build the zip: $($_.Exception.Message)"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    exit 1
}

# Read back, as with the test package. The lesson there was that every
# check which looked at an earlier step than the one that broke passed.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$a = [IO.Compression.ZipFile]::OpenRead($zip)
try { $have = $a.Entries | ForEach-Object { $_.Name } } finally { $a.Dispose() }
$missing = @("amdgpu-arch.exe", "hipInfo.exe", $hipRt.Name, "probe.ps1",
             "RUN-ME-first.bat", "README.txt") |
           Where-Object { $have -notcontains $_ }
if ($missing) {
    Write-Output "the zip is missing: $($missing -join ', ')"
    Remove-Item -Force $zip
    exit 1
}

Write-Output ""
Write-Output "zip: $zip"
Write-Output "  $((Get-Item $zip).Length) bytes, $($have.Count) files, HIP runtime $($hipRt.Name) from $Rocm"
