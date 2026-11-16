<#
.SYNOPSIS
    Builds what WoW players on Linux need: SpokenPlayersVoicepack-Linux.zip, holding
    SpokenPlayersVoicepack-Wine.exe (with sapifix.dll, signed, inside it), install.sh and README.md.
    By default into out\, as tools\Build-Installer.ps1 puts the setup.

.DESCRIPTION
    Needs Visual Studio's C++ build tools (MSVC and the Windows SDK: the "Desktop development with
    C++" workload, which the engine's Rust build needs too); found with vswhere. No administrator.

      1. Compiles sapifix.dll (sapifix.c, sapifix.def) with MSVC, the C runtime linked in (/MT), so it
         needs nothing in a Wine prefix but Wine.
      2. Signs it as engine\tools\Sign-Engine.ps1 signs the engine (WoW loads no SAPI DLL whose
         signature it does not trust): a self-signed certificate for code signing only, made for this
         build, its private key deleted straight after. Unlike the engine's, it is never trusted on
         this PC: SapiFix is for Wine prefixes only, where SpokenPlayersVoicepack-Wine.exe trusts it.
         Its public part (sapifix.cer) goes into the .exe.
      3. Compiles SpokenPlayersVoicepack-Wine.exe (setup.c, setup.rc: the icon, sapifix.dll and
         sapifix.cer as resources).
      4. Zips the .exe, install.sh and README.md into one folder, SpokenPlayersVoicepack-Linux.
         A zip made on Windows keeps no Unix permissions, so players run it with `bash install.sh`.

    What it builds is GPL-3.0-or-later, as the engine is: hand out its source with it (LICENSE).
    The intermediate files are in out\wine\ (not in git).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File wine\Build-Wine.ps1
    powershell -ExecutionPolicy Bypass -File wine\Build-Wine.ps1 -Out C:\temp\try
#>
param([string]$Out)

$ErrorActionPreference = 'Stop'
$Wine = $PSScriptRoot
$Root = Split-Path $Wine -Parent
$Build = Join-Path $Root 'out\wine'
$Obj = Join-Path $Build 'obj'
if (-not $Out) { $Out = Join-Path $Root 'out' }
$Out = [IO.Path]::GetFullPath([IO.Path]::Combine((Get-Location).Path, $Out))
$Subject = 'CN=Spoken Players SapiFix (local, this build only)'

# ---- MSVC: vcvars64.bat's environment, taken into this session ------------------------------------

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'Visual Studio (or its Build Tools) is not installed: vswhere.exe is missing.' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio with the C++ build tools (MSVC x64): add the "Desktop development with C++" workload.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
# Through a .cmd file of its own: cmd /c "<path with spaces>" && set loses the quotes.
$probe = Join-Path ([IO.Path]::GetTempPath()) "spoken-vcvars-$PID.cmd"
Set-Content $probe "@call `"$vcvars`" >nul 2>&1`r`n@set" -Encoding ASCII
try {
    foreach ($line in & cmd.exe /d /c $probe) {
        if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2]) }
    }
} finally {
    Remove-Item $probe -ErrorAction SilentlyContinue
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw "vcvars64.bat gave no cl.exe ($vcvars)" }
if (-not (Get-Command rc.exe -ErrorAction SilentlyContinue)) { throw 'No rc.exe: the Windows SDK is missing from the C++ build tools.' }

function Invoke-Tool([string]$tool, [string[]]$arguments) {
    & $tool @arguments
    if ($LASTEXITCODE) { throw "$tool failed (exit code $LASTEXITCODE)" }
}

New-Item -ItemType Directory -Force $Obj | Out-Null
# Warnings at level 3; a function called without a declaration is an error, never an int-returning
# guess (a pointer cut to 32 bits).
$cflags = @('/nologo', '/O2', '/MT', '/W3', '/we4013', '/std:c11', '/utf-8', '/D_CRT_SECURE_NO_WARNINGS')

# ---- 1. sapifix.dll ------------------------------------------------------------------------------

$dll = Join-Path $Build 'sapifix.dll'
$cer = Join-Path $Build 'sapifix.cer'
Push-Location $Wine
try {
    Invoke-Tool cl.exe ($cflags + @('/LD', 'sapifix.c', "/Fo$(Join-Path $Obj 'sapifix.obj')", "/Fe$dll",
        '/link', '/DEF:sapifix.def', "/IMPLIB:$(Join-Path $Obj 'sapifix.lib')",
        'ole32.lib', 'oleaut32.lib', 'uuid.lib', 'sapi.lib', 'shlwapi.lib', 'user32.lib', 'advapi32.lib'))
} finally {
    Pop-Location
}

# ---- 2. signed, with a certificate for this build only -------------------------------------------

# One left by a build that stopped half way: its key goes too.
Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $Subject } |
    ForEach-Object { Remove-Item $_.PSPath -DeleteKey }
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject -CertStoreLocation 'Cert:\CurrentUser\My' `
    -KeyExportPolicy NonExportable -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(10)
try {
    Export-Certificate -Cert $cert -FilePath $cer | Out-Null
    # Its status says the chain ends in a root this PC does not trust: as meant (step 2 above).
    Set-AuthenticodeSignature -FilePath $dll -Certificate $cert -HashAlgorithm SHA256 | Out-Null
} finally {
    Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -DeleteKey
}
$signer = (Get-AuthenticodeSignature -FilePath $dll).SignerCertificate
if (-not $signer -or $signer.Thumbprint -ne $cert.Thumbprint) { throw "sapifix.dll did not get signed" }
Write-Host "sapifix.dll signed: $($cert.Thumbprint) (private key deleted)"

# ---- 3. SpokenPlayersVoicepack-Wine.exe ----------------------------------------------------------

$exe = Join-Path $Build 'SpokenPlayersVoicepack-Wine.exe'
$res = Join-Path $Obj 'setup.res'
Push-Location $Wine  # setup.rc names its files from here (..\out\wine, ..\installer\art)
try {
    Invoke-Tool rc.exe @('/fo', $res, 'setup.rc')
    Invoke-Tool cl.exe ($cflags + @('setup.c', $res, "/Fo$(Join-Path $Obj 'setup.obj')", "/Fe$exe",
        '/link', '/SUBSYSTEM:CONSOLE',
        'ole32.lib', 'uuid.lib', 'sapi.lib', 'user32.lib', 'advapi32.lib', 'crypt32.lib', 'urlmon.lib', 'wininet.lib'))
} finally {
    Pop-Location
}

# ---- 4. the zip ----------------------------------------------------------------------------------

# install.sh runs in bash: Windows line ends would break it (.gitattributes keeps it LF).
if ((Get-Content (Join-Path $Wine 'install.sh') -Raw).Contains("`r")) {
    throw 'wine\install.sh has Windows line ends (CRLF): bash would fail on it. Check it out again with LF.'
}
New-Item -ItemType Directory -Force $Out | Out-Null
$zip = Join-Path $Out 'SpokenPlayersVoicepack-Linux.zip'
Remove-Item $zip -ErrorAction SilentlyContinue
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
# Entry by entry, named with '/': PowerShell 5's Compress-Archive writes '\', which unzip on Linux
# takes for part of the file's name.
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in @($exe, (Join-Path $Wine 'install.sh'), (Join-Path $Wine 'README.md'))) {
        $name = 'SpokenPlayersVoicepack-Linux/' + (Split-Path $file -Leaf)
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file, $name) | Out-Null
    }
} finally {
    $archive.Dispose()
}
Write-Host "`nBuilt $zip" -ForegroundColor Green
Write-Host 'On Linux: unzip it, then in its folder: bash install.sh' -ForegroundColor Cyan
