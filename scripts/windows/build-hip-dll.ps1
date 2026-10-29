# Build the kernels into avian_amd.dll with the HIP SDK, for AMD cards.
#
# The exported symbols are still called avian_cuda_*, which is not a mistake:
# the miner reaches a backend through the plain-data ABI in
# cuda/avian-cuda-api.h and that ABI has one set of names whichever card runs
# it. Only the file name differs, because the miner loads both DLLs rather
# than importing either, and two files cannot share a name in a directory.
#
# So an existing avian-miner.exe runs on AMD with no rebuild, and runs on
# both vendors at once if both DLLs are beside it. That is what the DLL split
# was for, even though it was not written with AMD in mind.
#
# What a tester needs is therefore AMD's display driver and nothing else: a
# 6.x build imports amdhip64_6.dll, which the driver installs. The HIP SDK is
# needed to build this, not to run it. A 7.x build imports amdhip64_7.dll
# instead, which no consumer driver has, so that one has to ship a runtime
# alongside. See the $Rocm note below.
#
# One architecture per --offload-arch, and unlike CUDA there is no PTX to
# fall back on: a card not listed here will not run the result. Build time
# scales with the list.
param(
    [switch]$NoPtx,
    # RX 6000, RX 7000 and RX 9000. A card outside this list will not launch
    # at all, there being no PTX to fall back on, so the default errs towards
    # covering consumer hardware rather than towards a quick build.
    #
    # gfx1031 and gfx1032 are the smaller RDNA2 parts, RX 6700 and RX 6600.
    # AMD's own Windows support for them has been going backwards: the
    # runtime row is a tick on the 5.6 through 7.1.1 pages and a cross on
    # 7.2.0, while the HIP SDK row is a cross throughout. Building for them
    # costs one pass each and makes the failure, if it comes, a clear message
    # from the runtime rather than a kernel that silently will not launch.
    #
    # The integrated and entry-level parts are gfx1033 to gfx1036, Van Gogh
    # and the Rembrandt and Raphael APUs, and gfx1150 to gfx1153, Strix and
    # Krackan. They will be slow wherever they work, and the value in having
    # them is a clear "ran correctly" rather than throughput. They were added
    # only once gfx1030 had hashed correctly on real hardware, so that a
    # failure on one of them is that part's failure rather than a question
    # about the whole port.
    #
    # Sixteen targets is roughly half an hour and a 40 MB DLL, at about 1.9
    # minutes and 2.3 MB each. -Arch takes a shorter list when that matters.
    [string[]]$Arch = @("gfx1030", "gfx1031", "gfx1032", "gfx1033",
                        "gfx1034", "gfx1035", "gfx1036",
                        "gfx1100", "gfx1101", "gfx1102",
                        "gfx1150", "gfx1151", "gfx1152", "gfx1153",
                        "gfx1200", "gfx1201"),
    # 6.x, and this is the setting that decides whether a consumer card works
    # at all. The runtime a DLL imports is fixed at link time by exact
    # versioned filename: built against 6.4 it imports amdhip64_6.dll, which
    # is what a current Adrenalin driver installs, and built against 7.x it
    # imports amdhip64_7.dll, which no consumer driver has. Both 7.x builds
    # reported no device on an RX 6900 XT whose driver was thirteen days old.
    # The 6.4 build hashes on it at 25 MH/s.
    #
    # AMD's support matrix is a separate question and a lesser one. 7.2.0
    # marks gfx1030 unsupported where 7.1.1 marks it supported, which is real
    # and was not the failure: a card missing from the list cannot launch a
    # kernel, rather than getting as far as reporting no device.
    #
    # 7.x is still reachable with -Rocm and needs its runtime shipped
    # alongside. 6.x must not ship one, because 6.4's amdhip64_6.dll imports
    # 121 MB of amd_comgr_2.dll statically where 7.1.1's loads comgr lazily.
    # See package-amd-test.ps1 -NoRuntime and doc/building.md.
    #
    # The 7.1 installer nests a second version directory inside the first, so
    # that one is ...\ROCm\7.1\7.1 rather than ...\ROCm\7.1.
    [string]$Rocm = "C:\Program Files\AMD\ROCm\6.4",
    # Named per SDK when building more than one, because the runtime a DLL
    # imports is fixed at link time: a 6.x build and a 7.x build are
    # different files rather than a choice made at run time.
    [string]$Out = $(if ($env:AVIAN_BUILD_DIR) { "$env:AVIAN_BUILD_DIR\hip" } else { Join-Path (Resolve-Path "$PSScriptRoot\..\..\..") "avian-winbuild\hip" })
)

# The repository, derived rather than named, so a clone anywhere
# works. $PSScriptRoot is scripts\windows.
$W = (Resolve-Path "$PSScriptRoot\..\..").Path
$OUT = $Out
$VCVARS = "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"

New-Item -ItemType Directory -Force $OUT | Out-Null

$clang = Join-Path $Rocm "bin\clang++.exe"
$hiplib = Join-Path $Rocm "lib\amdhip64.lib"
foreach ($p in @($clang, $hiplib, $VCVARS)) {
    if (-not (Test-Path $p)) { Write-Output "missing: $p"; exit 1 }
}

$cu = @()
$cu += Get-ChildItem -Path "$W\cuda" -Filter *.cu -Recurse | ForEach-Object { $_.FullName }
$cu += "$W\algo\x16\x16rt-cuda-batch.cu"

# The same two sphlib files the CUDA DLL carries: the kernels' host side calls
# them to precompute the 80-byte midstates, so they come along rather than
# becoming bridges back into the miner. They are C, so -x has to change for
# them; with clang an -x applies to every input after it until the next one.
$extraFiles = @("$W\algo\whirlpool\sph_whirlpool.c", "$W\algo\jh\sph_jh.c") |
              Where-Object { Test-Path $_ }

Write-Output "=== sources ==="
Write-Output "  $($cu.Count) .cu files, plus $($extraFiles.Count) C files"
Write-Output "  targets: $($Arch -join ', ')"
Write-Output "  SDK: $Rocm"

$list = ($cu | ForEach-Object { "`"$_`"" }) -join " "
$extra = ($extraFiles | ForEach-Object { "`"$_`"" }) -join " "
$archArgs = ($Arch | ForEach-Object { "--offload-arch=$_" }) -join " "
# Deliberately not defined. cuda/avian-ptx-portable.h sets AVIAN_NO_PTX itself
# under __HIPCC__, and defining it on the command line overrides that: passing
# =0 sent this build down the inline-PTX path and it failed on 'invalid output
# constraint' for every 64-bit asm in the tree. The define exists to force the
# portable bodies on nvcc, where they are not the default. Here they always
# are, so -NoPtx has nothing to switch and is accepted only so the two DLL
# scripts take the same arguments.
if ($NoPtx) { Write-Output "  -NoPtx ignored: a HIP build is always the portable path" }

# cuda\hip first on the include path, so a kernel's #include <cuda_runtime.h>
# finds the rename shim rather than a CUDA toolkit that may also be installed.
# -fms-runtime-lib=dll matches nvcc's /MD, so the DLL and the MinGW miner do
# not end up with two C runtimes between them.
#
# The CRT import libraries are named explicitly. -fms-runtime-lib=dll makes
# the compiler emit __imp_ references for getenv and strtoul, but clang does
# not pull the CRT in by itself for a -shared link, and the result is
# LNK2019 on those two alone. nvcc never showed this because it hands the
# host link to MSVC, which defaults them in.
# The two sphlib files are compiled on their own first. They are C, and
# -std=c++14 is an error on a C input, so they cannot share the kernels'
# command line. nvcc did not hit this because it drives separate host
# invocations itself.
$cObjs = @()
$cCmds = foreach ($c in $extraFiles) {
    $o = Join-Path $OUT ([IO.Path]::GetFileNameWithoutExtension($c) + ".obj")
    $cObjs += $o
    # -I"$W" as well, because these reach for compat/sph_types.h relative to
    # the top of the tree.
    "`"$clang`" -O3 -c -w -fms-runtime-lib=dll -I`"$W`" -x c `"$c`" -o `"$o`" 2>&1"
}
$cObjList = ($cObjs | ForEach-Object { "`"$_`"" }) -join " "

$bat = @"
call "$VCVARS" >nul 2>&1
cd /d "$W"
$($cCmds -join "`r`n")
"$clang" -O3 -shared -o "$OUT\avian_amd.dll" ^
  -DAVIAN_CUDA_BUILDING_DLL ^
  $archArgs -std=c++14 -w ^
  -fms-runtime-lib=dll ^
  -I"$W\cuda\hip" -I"$W" -I"$W\cuda" -I"$W\cuda\upstream" ^
  -x hip $list ^
  -x none $cObjList "$hiplib" ^
  -lucrt -lmsvcrt -lvcruntime 2>&1
echo hipcc_exit=%ERRORLEVEL%
"@
$bat | Set-Content -Encoding ASCII "$OUT\build.bat"

Write-Output ""
Write-Output "=== compiling (one pass per architecture, so this is slow) ==="
$log = & cmd /c "`"$OUT\build.bat`"" 2>&1
$log | Select-String -Pattern "error|hipcc_exit" | Select-Object -First 20 |
    ForEach-Object { "  $_" }

if (Test-Path "$OUT\avian_amd.dll") {
    Write-Output ""
    Write-Output "DLL: $((Get-Item "$OUT\avian_amd.dll").Length) bytes"
    $vs = "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC"
    $dumpbin = Get-ChildItem -Path $vs -Filter dumpbin.exe -Recurse -ErrorAction SilentlyContinue |
               Select-Object -First 1
    if ($dumpbin) {
        Write-Output "=== exported entry points ==="
        & $dumpbin.FullName /exports "$OUT\avian_amd.dll" 2>&1 |
            Select-String -Pattern "avian_cuda_" | ForEach-Object { "  $_" }
    }
} else {
    Write-Output "no DLL produced; full log at $OUT\build.log"
    $log | Set-Content "$OUT\build.log"
}
