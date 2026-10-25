#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Signs ..\dist\spoken_voice.dll so WoW loads it: the game only loads SAPI voice engines whose
    DLL carries a trusted code signature (Microsoft's do; ours is unsigned until this runs).

.DESCRIPTION
    Makes a self-signed certificate good for code signing only, signs the DLL with it, and trusts the
    certificate on this computer (Trusted Root and Trusted Publishers). Then it deletes the certificate's
    private key: nothing else can ever be signed with it, by anyone. After rebuilding the engine, run
    this again; it makes a new certificate and takes the old ones out.

    -Remove takes every certificate this script made out of the trust stores again.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\Sign-Engine.ps1
    powershell -ExecutionPolicy Bypass -File .\Sign-Engine.ps1 -Remove
#>
param([switch]$Remove)

$ErrorActionPreference = 'Stop'
$Subject = 'CN=Spoken Voice Engine (local, this PC only)'
$Dll = Join-Path (Split-Path $PSScriptRoot -Parent) 'dist\spoken_voice.dll'

function Remove-OurCertificates {
    foreach ($store in 'Cert:\LocalMachine\Root', 'Cert:\LocalMachine\TrustedPublisher', 'Cert:\CurrentUser\My') {
        Get-ChildItem $store | Where-Object { $_.Subject -eq $Subject } | ForEach-Object {
            Write-Host "removing $($_.Thumbprint) from $store"
            Remove-Item $_.PSPath -DeleteKey -ErrorAction SilentlyContinue
            if (Test-Path $_.PSPath) { Remove-Item $_.PSPath }
        }
    }
}

Remove-OurCertificates
if ($Remove) { return }
if (-not (Test-Path $Dll)) { throw "missing ${Dll}" }

# 1. A certificate for code signing only, its key kept non-exportable.
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject -CertStoreLocation 'Cert:\CurrentUser\My' `
    -KeyExportPolicy NonExportable -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(10)
Write-Host "certificate $($cert.Thumbprint)"

# 2. Trusted on this computer: as a root, and as a publisher. Its public part goes beside the engine
#    (dist\spoken_voice_engine.cer): the installer's build checks the engine against it, and the
#    setup has the PCs it installs on trust it.
$public = Join-Path (Split-Path $Dll -Parent) 'spoken_voice_engine.cer'
Export-Certificate -Cert $cert -FilePath $public | Out-Null
foreach ($store in 'Cert:\LocalMachine\Root', 'Cert:\LocalMachine\TrustedPublisher') {
    Import-Certificate -FilePath $public -CertStoreLocation $store | Out-Null
}

# 3. Sign the engine.
$signature = Set-AuthenticodeSignature -FilePath $Dll -Certificate $cert -HashAlgorithm SHA256
Write-Host "signed: $($signature.Status) $($signature.StatusMessage)"

# 4. Throw the private key away: the trusted certificate can sign nothing else, ever.
Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -DeleteKey
Write-Host 'private key deleted'

$check = Get-AuthenticodeSignature -FilePath $Dll
Write-Host "`n$Dll : $($check.Status)" -ForegroundColor $(if ($check.Status -eq 'Valid') { 'Green' } else { 'Red' })
Write-Host 'Restart WoW, then try a Spoken voice.' -ForegroundColor Cyan
