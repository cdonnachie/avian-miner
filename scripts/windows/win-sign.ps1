# Sign the Windows artifacts this repository builds.
#
# Run after win-package.sh, because that stage copies files and nothing
# rewrites a binary afterwards. Signing earlier works too, since a copy
# carries the signature, but then the order has to be remembered.
#
# Only our own artifacts are signed. The MSYS2 runtime DLLs beside them are
# other people's code, and signing those with your certificate asserts that
# you stand behind OpenSSL and libcurl, which is not what a signature on a
# miner is meant to say. They ship unsigned, as they arrive.
#
# Both the miner and its backends are signed, which matters more since the
# backends stopped being linked: the kernels are a run-time LoadLibrary now,
# so a signature on the exe alone attests nothing about the code that does
# every hash.
param(
    # Defaults to the one code signing certificate in the personal store.
    # Not written out, because this script is tracked and a thumbprint is
    # one person's.
    [string] $Thumbprint,
    # RFC3161. Certum's, matching the Open Source Developer certificates
    # this project is signed with; override for another issuer.
    [string] $TimestampUrl = 'http://time.certum.pl',
    # The package directory. Derived like the other stages, so the
    # PowerShell and MSYS2 halves agree on where each other's output went.
    [string] $Dir
)

$ErrorActionPreference = 'Stop'

if (-not $Dir) {
    $root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $Dir = Join-Path (Split-Path -Parent $root) 'avian-winbuild\package'
}
if (-not (Test-Path $Dir)) { throw "no package directory at $Dir; run win-package.sh first" }

# The newest SDK, not whichever one is first on PATH. signtool is not on PATH
# at all on a normal install, which is why this looks rather than assuming.
$signtool = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe' -ErrorAction SilentlyContinue |
            Sort-Object { [version]($_.Directory.Parent.Name) } | Select-Object -Last 1
if (-not $signtool) { throw 'no signtool.exe under the Windows 10/11 SDK' }

if (-not $Thumbprint) {
    $certs = @(Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert | Where-Object { $_.HasPrivateKey })
    if ($certs.Count -ne 1) {
        throw "expected one usable code signing certificate, found $($certs.Count); pass -Thumbprint"
    }
    $Thumbprint = $certs[0].Thumbprint
    Write-Host "certificate: $($certs[0].Subject)"
}

# A certificate outlives none of its signatures only if they are timestamped.
# Without this every binary signed here stops validating the day the
# certificate expires, rather than remaining valid as of signing time.
$cert = Get-Item "Cert:\CurrentUser\My\$Thumbprint" -ErrorAction SilentlyContinue
if ($cert) { Write-Host ("expires:     {0:yyyy-MM-dd}" -f $cert.NotAfter) }

$ours = 'avian-miner.exe', 'avian_cuda.dll', 'avian_amd.dll'
$targets = $ours | ForEach-Object { Join-Path $Dir $_ } | Where-Object { Test-Path $_ }
if (-not $targets) { throw "none of $($ours -join ', ') found in $Dir" }

Write-Host "`n=== signing ==="
foreach ($t in $targets) { Write-Host "  $(Split-Path -Leaf $t)" }

& $signtool.FullName sign /sha1 $Thumbprint /fd SHA256 /tr $TimestampUrl /td SHA256 /v @targets
if ($LASTEXITCODE -ne 0) { throw "signtool sign failed ($LASTEXITCODE)" }

# /pa uses the Authenticode policy rather than the default driver policy,
# which a code signing certificate does not satisfy and which would report a
# correctly signed binary as untrusted.
Write-Host "`n=== verifying ==="
& $signtool.FullName verify /pa /v @targets
if ($LASTEXITCODE -ne 0) { throw "signtool verify failed ($LASTEXITCODE)" }

Write-Host "`nsigned and verified: $($targets.Count) file(s) in $Dir"
