# Build the kernels into avian_cuda.dll with the HIP SDK, for AMD cards.
#
# The name is not a mistake. The miner reaches the backend through the
# plain-data ABI in cuda/avian-cuda-api.h, every symbol of which is called
# avian_cuda_* whichever card runs it, and it loads that ABI from a DLL of
# this name. Building the HIP kernels into the same shape means an existing
# avian-miner.exe runs on AMD with no rebuild: swap the DLL and nothing else
# changes. That is what the DLL split was for, even though it was not written
# with AMD in mind.
#
# What a tester needs is therefore only AMD's display driver, which ships
# amdhip64_7.dll. The HIP SDK is needed to build this, not to run it.
#
# One architecture per --offload-arch, and unlike CUDA there is no PTX to
# fall back on: a card not listed here will not run the result. Build time
# scales with the list.
param(
    [switch]$NoPtx,
    [string[]]$Arch = @("gfx1030", "gfx1100", "gfx1101", "gfx1102"),
    [string]$Rocm = "C:\Program Files\AMD\ROCm\7.2"
)

$W = "C:\development\cpuminer-avian"
$OUT = "C:\Users\craig\AppData\Local\Temp\claude\c--development-Avian-v5-0\9e1a4f58-d2d0-4ac5-b59e-ba1e8ee54635\scratchpad\winbuild\hip"
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
"$clang" -O3 -shared -o "$OUT\avian_cuda.dll" ^
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

if (Test-Path "$OUT\avian_cuda.dll") {
    Write-Output ""
    Write-Output "DLL: $((Get-Item "$OUT\avian_cuda.dll").Length) bytes"
    $vs = "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC"
    $dumpbin = Get-ChildItem -Path $vs -Filter dumpbin.exe -Recurse -ErrorAction SilentlyContinue |
               Select-Object -First 1
    if ($dumpbin) {
        Write-Output "=== exported entry points ==="
        & $dumpbin.FullName /exports "$OUT\avian_cuda.dll" 2>&1 |
            Select-String -Pattern "avian_cuda_" | ForEach-Object { "  $_" }
    }
} else {
    Write-Output "no DLL produced; full log at $OUT\build.log"
    $log | Set-Content "$OUT\build.log"
}
