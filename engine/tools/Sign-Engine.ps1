<#
.SYNOPSIS
    Signs ..\dist\spoken_voice.dll so WoW loads it: the game only loads SAPI voice engines whose DLL
    carries a code signature (ours has none until this runs).

.DESCRIPTION
    Makes a self-signed certificate good for code signing only, signs the DLL with it, then deletes the
    certificate's private key: nothing else can ever be signed with it. Nothing is trusted: WoW wants
    the engine signed, not its signature trusted (tested in retail and WoW Forever, 2026-11-17: an
    engine whose certificate no PC trusts speaks, an unsigned one doesn't). No administrator needed.
    After rebuilding the engine, run this again.

    Run as administrator, it also takes out of the machine's trust stores (Root, TrustedPublisher)
    every engine certificate this script or an older setup once had this PC trust. -Remove does only
    that, and signs nothing.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\Sign-Engine.ps1
    powershell -ExecutionPolicy Bypass -File .\Sign-Engine.ps1 -Remove   (as administrator)
#>
param([switch]$Remove)

$ErrorActionPreference = 'Stop'
$Subject = 'CN=Spoken Voice Engine (signed, never trusted)'
$Dll = Join-Path (Split-Path $PSScriptRoot -Parent) 'dist\spoken_voice.dll'
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

# The engine's certificates (by any of the names they had) that this PC trusts: out.
function Remove-TrustedCertificates {
    foreach ($store in 'Cert:\LocalMachine\Root', 'Cert:\LocalMachine\TrustedPublisher') {
        Get-ChildItem $store | Where-Object { $_.Subject -like '*Spoken Voice Engine*' } | ForEach-Object {
            Write-Host "no longer trusted: $($_.Thumbprint) ($store)"
            Remove-Item $_.PSPath
        }
    }
}

if ($Remove) {
    if (-not $admin) { throw 'Run -Remove as administrator: the trust stores are the machine''s.' }
    Remove-TrustedCertificates
    return
}
if (-not (Test-Path $Dll)) { throw "missing ${Dll}" }
if ($admin) { Remove-TrustedCertificates }

# 1. A certificate for code signing only, its key kept non-exportable (one left by a run that stopped
#    half way goes first, key and all).
Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $Subject } |
    ForEach-Object { Remove-Item $_.PSPath -DeleteKey }
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject -CertStoreLocation 'Cert:\CurrentUser\My' `
    -KeyExportPolicy NonExportable -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(10)
try {
    # 2. Sign the engine. Its status says the chain ends in a root this PC doesn't trust: as meant.
    Set-AuthenticodeSignature -FilePath $Dll -Certificate $cert -HashAlgorithm SHA256 | Out-Null
} finally {
    # 3. Throw the private key away: the certificate can sign nothing else, ever.
    Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -DeleteKey
}

$check = Get-AuthenticodeSignature -FilePath $Dll
$signed = $check.SignerCertificate -and $check.SignerCertificate.Thumbprint -eq $cert.Thumbprint
Write-Host "`n${Dll}: $(if ($signed) { "signed ($($cert.Thumbprint), private key deleted)" } else { "NOT signed ($($check.Status))" })" `
    -ForegroundColor $(if ($signed) { 'Green' } else { 'Red' })
if (-not $signed) { exit 1 }
Write-Host 'Rebuild the setup and install it with WoW closed, or copy the DLL over the installed one; then start WoW.' -ForegroundColor Cyan
