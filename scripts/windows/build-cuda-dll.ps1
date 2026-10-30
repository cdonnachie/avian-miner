# Build avian_cuda.dll: the kernels, with a C ABI, for the MinGW miner.
#
# nvcc will only use MSVC as its host compiler, while the miner and its Rust
# Stratum v2 library are MinGW, so this is the split that makes a Windows GPU
# build possible at all. The boundary carries plain data only: see
# cuda/avian-cuda-api.h for why.
#
# Compiles every .cu in one nvcc invocation rather than object by object, so
# device linking is implicit and there is no separate -dlink step to get
# wrong.

# -NoPtx builds the plain-C form of every lop3.b32 site instead of the inline
# PTX, which is what hipcc would select on its own. It exists so that the
# portable path is verifiable on the hardware we have: build with it, run the
# forced-order stage bisect, and 0 differing is evidence that the plain-C
# forms are hash-identical to the asm. See scripts/make-ptx-portable.py.
param([switch]$NoPtx)

$ErrorActionPreference = "Stop"
# The repository, derived rather than named, so a clone anywhere
# works. $PSScriptRoot is scripts\windows.
$W = (Resolve-Path "$PSScriptRoot\..\..").Path
$BUILD = $(if ($env:AVIAN_BUILD_DIR) { "$env:AVIAN_BUILD_DIR" } else { Join-Path (Resolve-Path "$PSScriptRoot\..\..\..") "avian-winbuild" })
$OUT = Join-Path $BUILD "cuda"
$CUDA = "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9"
$VCVARS = "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"

New-Item -ItemType Directory -Force -Path $OUT | Out-Null
# The support file is NOT copied in from the scratchpad. It used to be, and
# that one line overwrote the staged copy with a stale one on every DLL
# build, so the DLL kept a version of avian_cuda_device_count that never
# called cuda_detect_devices. device_sm then stayed 0, Keccak's launch was
# rejected for exceeding its own launch bound, and every chain containing
# Keccak hashed the previous stage's value. prep-windows-cuda.sh stages
# cuda/** from the WSL tree, which is the authority, so nothing is copied.

# Every kernel, the device helper, the batch driver and the support file.
$cu = @()
$cu += Get-ChildItem -Path "$W\cuda" -Filter *.cu -Recurse |
       ForEach-Object { $_.FullName }
$cu += "$W\algo\x16\x16rt-cuda-batch.cu"
Write-Output "=== sources ==="
Write-Output "  $($cu.Count) .cu files"

# Two sphlib files are plain C and self-contained. The kernels' host side
# calls them to precompute the 80-byte midstates, so they come along rather
# than becoming bridges back into the miner: whirlpool for the table setup
# and jh for jh512_setBlock_80.
$extraFiles = @("$W\algo\whirlpool\sph_whirlpool.c", "$W\algo\jh\sph_jh.c") |
              Where-Object { Test-Path $_ }
foreach ($e in $extraFiles) { Write-Output "  plus $(Split-Path $e -Leaf)" }
$extra = ($extraFiles | ForEach-Object { "`"$_`"" }) -join " "

$list = ($cu | ForEach-Object { "`"$_`"" }) -join " "
# Always passed explicitly, rather than left out when off, so the nvcc line
# never gains an empty continuation and the build is the same shape either way.
$ptxDef = if ($NoPtx) { "-DAVIAN_NO_PTX=1" } else { "-DAVIAN_NO_PTX=0" }
Write-Output "  $ptxDef"

# The architectures, from cuda/sm-targets, which configure.ac reads too so
# this DLL and libavian_cuda.so cover the same cards. Until this read them,
# the DLL was built for sm_86 alone plus compute_89 PTX, and PTX only works
# forwards: every card older than Ampere got "no kernel image is available
# for execution on the device" while the Linux build ran on all of them.
$smFile = Join-Path $PSScriptRoot "..\..\cuda\sm-targets"
if (-not (Test-Path $smFile)) { Write-Error "no architecture list at $smFile"; exit 1 }
$want = Get-Content $smFile |
    ForEach-Object { ($_ -replace '#.*', '').Trim() } | Where-Object { $_ }
$avail = & "$CUDAin
vcc.exe" --list-gpu-code 2>$null |
    ForEach-Object { $_.Trim() -replace '^sm_', '' } | Where-Object { $_ }
$built = @($want | Where-Object { $avail -contains $_ })
if (-not $built) { Write-Error "this toolkit emits none of: $($want -join ', ')"; exit 1 }
$skipped = @($want | Where-Object { $avail -notcontains $_ })
if ($skipped) { Write-Output "  not in this toolkit, skipped: $($skipped -join ', ')" }
# PTX for the newest that survived, so a card released after this build still
# runs by driver JIT rather than failing to launch.
$newest = $built[-1]
$arch = (($built | ForEach-Object { "-gencode=arch=compute_$_,code=sm_$_" }) +
         "-gencode=arch=compute_$newest,code=compute_$newest") -join " "
Write-Output "  targets: sm_$($built -join ', sm_'), plus compute_$newest as PTX"
$bat = @"
call "$VCVARS" >nul 2>&1
cd /d "$W"
"$CUDA\bin\nvcc.exe" -O3 -shared -o "$OUT\avian_cuda.dll" ^
  -DAVIAN_CUDA_BUILDING_DLL $ptxDef ^
  $arch ^
  -std=c++14 -w ^
  -I"$W" -I"$W\cuda" -I"$W\cuda\upstream" ^
  -Xcompiler "/MD /wd4819" ^
  $list $extra 2>&1
echo nvcc_exit=%ERRORLEVEL%
"@
$bat | Set-Content -Encoding ASCII "$OUT\build.bat"

Write-Output ""
Write-Output "=== nvcc (this takes a few minutes for 31 translation units) ==="
$log = & cmd.exe /c "`"$OUT\build.bat`"" 2>&1
$log | Select-String -Pattern "error|nvcc_exit" | Select-Object -First 20 |
    ForEach-Object { "  $_" }

if (Test-Path "$OUT\avian_cuda.dll") {
    Write-Output ""
    Write-Output "DLL: $((Get-Item "$OUT\avian_cuda.dll").Length) bytes"
    Write-Output "=== exported entry points ==="
    & cmd.exe /c "call `"$VCVARS`" >nul 2>&1 && dumpbin /exports `"$OUT\avian_cuda.dll`"" 2>&1 |
        Select-String -Pattern "avian_cuda" | ForEach-Object { "  $($_.ToString().Trim())" }
} else {
    Write-Output ""
    Write-Output "DLL NOT BUILT; last lines of the log:"
    $log | Select-Object -Last 15 | ForEach-Object { "  $_" }
}
