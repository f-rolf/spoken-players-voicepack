<#
.SYNOPSIS
    Builds the voicepack's installer into out\ (or -Out): SpokenPlayersVoicepack-Setup.exe, and
    voices.txt, the names of the voice models it downloads from -Url (voices/<model>-<hash>.onnx).
    README.md says how to build and sign everything it needs.

.DESCRIPTION
    Needs Inno Setup 6 (ISCC.exe) and the signed engine (engine\dist\spoken_voice.dll,
    engine\tools\Sign-Engine.ps1). Takes every voice, each from its pack in engine\dist\voices-multi
    (model.onnx, tokens.txt and voice.json in a folder per model, as published) when it is there,
    else from engine\dist\voices-info, and:
      - names each model's download for its contents, so a changed model is a new file and an
        unchanged one stays as it was
      - writes installer\build\voices.iss: a component per race (grouped by faction) and, per model,
        its files and its Windows voice's registry key (one voice per model, its speakers picked by the
        addon with a code in the text: engine\tools\VoiceNames.ps1 says why and what they are called)
      - compiles the setup (installer\SpokenPlayersVoicepack.iss), its version the build's date and time
    Not in it: the addon, which is installed on its own (from CurseForge).

    The setup takes the models from a voices\ folder beside it when there is one (as in
    out\, so it installs from there without a server), else from -Url.

    engine\dist\voices-info (in git) describes the published models: each one's voice.json, tokens.txt
    and model.onnx.sha256 (its SHA-256 and size), all the build needs of a model not packed on this PC
    (tools/voice-info.sh writes it from the packs), so a setup built here asks for the models packed
    here, and for the published ones for the rest.

.EXAMPLE
    .\Build-Installer.ps1
    Build Installer.cmd -Packs engine\dist\voices-multi -Url https://spoken-players.example/download/
        (only the models packed here, the setup only installing from the voices\ folder beside it: to try it)
    .\Build-Installer.ps1 -Voices orc_male,orc_female,leaders -Out C:\temp\try
#>
param(
    # Where the server serves the download folder: the setup downloads the models from its voices/.
    [string]$Url = 'https://spokenplayers.sahnet.ee/download/',
    [string]$Packs,
    [string]$Out,
    [string[]]$Voices,
    [string]$Version = "$(Get-Date -Format 'yyyy.M.d').$([int](Get-Date -Format 'HHmm'))"
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
. (Join-Path $Root 'engine\tools\VoiceNames.ps1')
$Dist = Join-Path $Root 'engine\dist'
$Installer = Join-Path $Root 'installer'
$Build = Join-Path $Installer 'build'
# Each model from its pack here (voices-multi) when it has one, else as published (voices-info, in
# git). -Packs: from that one folder only.
$Sources = $(if ($Packs) { @($Packs) } else { @((Join-Path $Dist 'voices-multi'), (Join-Path $Dist 'voices-info')) })
function Test-Files([string]$dir, [string[]]$files) {
    -not ($files | Where-Object { -not (Test-Path (Join-Path $dir $_)) })
}
$Folders = [ordered]@{}
foreach ($source in $Sources) {
    foreach ($folder in Get-ChildItem $source -Directory -ErrorAction SilentlyContinue) {
        $usable = (Test-Files $folder.FullName 'model.onnx', 'tokens.txt', 'voice.json') -or
                  (Test-Files $folder.FullName 'model.onnx.sha256', 'tokens.txt', 'voice.json')
        if ($usable -and -not $Folders.Contains($folder.Name)) { $Folders[$folder.Name] = $folder }
    }
}
if (-not $Out) { $Out = Join-Path $Root 'out' }
$Out = [IO.Path]::GetFullPath([IO.Path]::Combine((Get-Location).Path, $Out))  # Inno Setup's /O as well
if (-not $Url.EndsWith('/')) { $Url += '/' }

# The installer's components: the races by faction, then the voices that are no race.
$Factions = [ordered]@{
    alliance = @('Alliance', @('human', 'dwarf', 'nightelf', 'gnome', 'draenei', 'worgen', 'voidelf', 'darkiron', 'kultiran', 'mechagnome'))
    horde    = @('Horde', @('orc', 'undead', 'tauren', 'troll', 'bloodelf', 'goblin', 'nightborne', 'zandalari', 'vulpera'))
    both     = @('Both factions', @('pandaren', 'dracthyr', 'earthen', 'skyborne'))
    raid     = @('Raid voices', @('leaders', 'murloc'))
}
$OtherTitles = @{ leaders = 'Leaders, for the raid leader and raid warnings'; murloc = 'Murlocs' }

function Find-Iscc {
    foreach ($dir in "$env:LOCALAPPDATA\Programs\Inno Setup 6", "${env:ProgramFiles(x86)}\Inno Setup 6", "$env:ProgramFiles\Inno Setup 6") {
        if (Test-Path "$dir\ISCC.exe") { return "$dir\ISCC.exe" }
    }
    throw 'Inno Setup 6 is not installed (https://jrsoftware.org/isdl.php)'
}

# A value as an Inno Setup script takes it in quotes: constants' braces doubled, quotes doubled.
function Quote([string]$text) { '"' + $text.Replace('{', '{{').Replace('"', '""') + '"' }

# 1. What goes in.
$iscc = Find-Iscc
$dll = Join-Path $Dist 'spoken_voice.dll'
if ((Get-Item $dll).Length -lt 100KB) { throw "the engine is only Git LFS's placeholder: run 'git lfs pull'" }
# Signed by the engine's certificate (engine\dist\spoken_voice_engine.cer) and unchanged since. This PC
# need not trust that certificate (UnknownError: its root is not trusted here); the setup makes the
# PCs it installs on trust it.
$signature = Get-AuthenticodeSignature $dll
$engineCert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 (Join-Path $Dist 'spoken_voice_engine.cer')
if ($signature.Status -notin 'Valid', 'UnknownError' -or $signature.SignerCertificate.Thumbprint -ne $engineCert.Thumbprint) {
    throw "spoken_voice.dll is not signed by the engine's certificate ($($signature.Status)): run engine\tools\Sign-Engine.ps1 as administrator first"
}
$models = @()
foreach ($folder in $Folders.Values | Sort-Object Name) {
    if ($Voices -and $folder.Name -notin $Voices) { continue }
    $model = Join-Path $folder.FullName 'model.onnx'
    $info = "$model.sha256"  # voices-info's, for a model not packed on this PC
    $small = @('tokens.txt', 'voice.json') | Where-Object { Test-Path (Join-Path $folder.FullName $_) }
    if ($small.Count -lt 2 -or -not ((Test-Path $model) -or (Test-Path $info))) {
        Write-Warning "leaving out $($folder.Name): its pack is incomplete"; continue
    }
    $spoken = @(Get-SpokenVoices $folder.FullName)
    if (-not $spoken) { continue }
    $group = $(if ($OtherModels.ContainsKey($folder.Name)) { $folder.Name } else { ($folder.Name -split '_')[0] })
    $faction = @($Factions.Keys | Where-Object { $group -in $Factions[$_][1] })[0]
    if (-not $faction) { throw "$($folder.Name): no faction for '$group' (add it to `$Factions)" }
    if (Test-Path $model) {
        $hash, $size = (Get-FileHash $model -Algorithm SHA256).Hash.ToLower(), (Get-Item $model).Length
    } else {
        $hash, $size = (Get-Content $info -TotalCount 1).Trim() -split '\s+'
        if ($hash -notmatch '^[0-9a-f]{64}$' -or $size -notmatch '^\d+$') { throw "$info is not '<sha256> <size>'" }
        $model = $null
    }
    $models += [pscustomobject]@{ Name = $folder.Name; Folder = $folder.FullName; Voices = $spoken; Group = $group
                                  Component = "$faction\$group"; Hash = $hash; Size = [long]$size; Model = $model
                                  File = "$($folder.Name)-$($hash.Substring(0, 12)).onnx" }
}
if ($models.Count -eq 0) { throw "no finished voices in $($Sources -join ' or ')" }

# 2. The models' downloads: new ones copied, ones no longer in the setup removed. voices.txt lists them
#    (model, download, SHA-256): what the site must serve (spoken-players-site's tools/publish-voices.sh).
New-Item (Join-Path $Out 'voices') -ItemType Directory -Force | Out-Null
foreach ($m in $models | Where-Object Model) {
    $to = Join-Path $Out "voices\$($m.File)"
    if (-not (Test-Path $to)) { Copy-Item $m.Model $to }
}
Get-ChildItem (Join-Path $Out 'voices') -File | Where-Object { $_.Name -notin $models.File } | Remove-Item
$list = ($models | ForEach-Object { "$($_.Name) $($_.File) $($_.Hash)`n" }) -join ''
[IO.File]::WriteAllText((Join-Path $Out 'voices.txt'), $list, (New-Object Text.ASCIIEncoding))
$elsewhere = @($models | Where-Object { -not $_.Model })

# 3. voices.iss: the components, each model's files, and its voice's registry key.
if (Test-Path $Build) { Remove-Item $Build -Recurse -Force }
New-Item $Build -ItemType Directory | Out-Null
$iss = [Collections.Generic.List[string]]::new()
$iss.Add('; Written by tools\Build-Installer.ps1 for this build: do not edit.')
$iss.Add("#define Models `"$($models.Name -join ',')`"")
$iss.Add("#define Components `"$(@($models.Component | Select-Object -Unique) -join ',')`"")
$iss.Add("#define ModelComponents `"$(($models | ForEach-Object { "$($_.Name)=$($_.Component)" }) -join ',')`"")
$iss.Add('')
$iss.Add('[Components]')
foreach ($faction in $Factions.Keys) {
    $in = @($models | Where-Object { $_.Component -like "$faction\*" })
    if (-not $in) { continue }
    $iss.Add("Name: `"$faction`"; Description: $(Quote $Factions[$faction][0]); Types: full custom")
    foreach ($group in $Factions[$faction][1]) {
        $of = @($in | Where-Object Group -eq $group)
        if (-not $of) { continue }
        $title = $(if ($OtherTitles.ContainsKey($group)) { $OtherTitles[$group] } else { $RaceNames[$group] })
        $count = ($of | ForEach-Object { $_.Voices.Count } | Measure-Object -Sum).Sum
        $iss.Add("Name: `"$faction\$group`"; Description: $(Quote "$title ($count voices)"); Types: full custom")
    }
}
$iss.Add('')
$iss.Add('[Files]')
foreach ($m in $models) {
    $into = "{app}\voices\$($m.Name)"
    foreach ($small in 'tokens.txt', 'voice.json') {
        $iss.Add("Source: $(Quote (Join-Path $m.Folder $small)); DestDir: `"$into`"; Components: $($m.Component); Flags: ignoreversion")
    }
    $model = "DestDir: `"$into`"; DestName: `"model.onnx`"; Components: $($m.Component); Hash: `"$($m.Hash)`"; ExternalSize: $($m.Size)"
    $saved = "AfterInstall: ModelInstalled('$($m.Name)', '$($m.Hash)')"
    $iss.Add("Source: `"{src}\voices\$($m.File)`"; $model; Check: ModelFromDisk('$($m.Name)', '$($m.Hash)', '$($m.File)'); $saved; Flags: external ignoreversion")
    $iss.Add("Source: `"$Url" + "voices/$($m.File)`"; $model; Check: ModelFromWeb('$($m.Name)', '$($m.Hash)', '$($m.File)'); $saved; Flags: external download ignoreversion")
}
$iss.Add('')
$iss.Add('[InstallDelete]')
foreach ($m in $models) { $iss.Add("Type: filesandordirs; Name: `"{app}\voices\$($m.Name)`"; Components: not $($m.Component)") }
$iss.Add('')
$iss.Add('[Registry]')
foreach ($m in $models) {
    # One voice per model, its first speaker by default (SpeakerId): the setup's RemoveSpokenVoices takes
    # an older install's voice per speaker (SpokenGroup_<model>_<n>) away first.
    $voice = Get-ModelVoice $m.Folder
    $key = "Root: HKLM; Subkey: `"{#Tokens}\SpokenPlayers_$($m.Name)"
    $only = "Components: $($m.Component)"
    $values = [ordered]@{ '' = $voice.Name; '409' = $voice.Name; 'CLSID' = '{#Clsid}'
                          'ModelDir' = "{app}\voices\$($m.Name)"; 'DataDir' = '{app}\espeak-ng-data' }
    $first = $true
    foreach ($name in $values.Keys) {
        $data = $(if ($values[$name] -like '{*') { "`"$($values[$name])`"" } else { Quote $values[$name] })
        $flags = $(if ($first) { '; Flags: uninsdeletekey' } else { '' })
        $iss.Add("$key`"; ValueType: string; ValueName: `"$name`"; ValueData: $data; $only$flags")
        $first = $false
    }
    $iss.Add("$key`"; ValueType: dword; ValueName: `"SpeakerId`"; ValueData: 0; $only")
    $iss.Add("$key`"; ValueType: dword; ValueName: `"SpokenGroupVoice`"; ValueData: 1; $only")
    $attributes = [ordered]@{ Name = $voice.Name; Gender = $voice.Gender; Age = 'Adult'; Language = '409'; Vendor = 'Spoken Players' }
    foreach ($name in $attributes.Keys) {
        $iss.Add("$key\Attributes`"; ValueType: string; ValueName: `"$name`"; ValueData: $(Quote $attributes[$name]); $only")
    }
}
[IO.File]::WriteAllLines((Join-Path $Build 'voices.iss'), $iss, (New-Object Text.UTF8Encoding $true))

# 4. The engine's certificate, from its signature, for the PCs it is installed on to trust.
Export-Certificate -Cert $signature.SignerCertificate -FilePath (Join-Path $Build 'spoken_voice_engine.cer') | Out-Null

# 5. The setup.
$arguments = @('/Q', "/O$Out", "/DVersion=$Version", "/DUrl=$Url", "/DDist=$Dist", "/DBuild=$Build",
               "/DThumbprint=$($signature.SignerCertificate.Thumbprint)", (Join-Path $Installer 'SpokenPlayersVoicepack.iss'))
& $iscc @arguments
if ($LASTEXITCODE -ne 0) { throw "Inno Setup could not compile the setup (exit code $LASTEXITCODE)" }
# What the update check of earlier builds read: gone with it.
Remove-Item (Join-Path $Out 'latest.txt') -ErrorAction SilentlyContinue

$setup = Get-Item (Join-Path $Out 'SpokenPlayersVoicepack-Setup.exe')
$size = ($models | Measure-Object Size -Sum).Sum
$count = ($models | ForEach-Object { $_.Voices.Count } | Measure-Object -Sum).Sum
$here = @($models | Where-Object Model)
Write-Host ''
Write-Host "Built: Spoken Players Voicepack $Version, $($models.Count) models (a Windows voice each), $count speakers" -ForegroundColor Green
Write-Host "  setup:  $($setup.FullName) ($([Math]::Round($setup.Length / 1MB, 1)) MB)"
Write-Host "  models: $([Math]::Round($size / 1GB, 2)) GB, downloaded by the setup from ${Url}voices/, named in voices.txt" -ForegroundColor $(if ($Url -like '*.example/*') { 'Yellow' } else { 'Gray' })
if ($Url -like '*.example/*') {
    Write-Host '  (a placeholder: build with -Url <where out\ is served> before publishing;' -ForegroundColor Yellow
    Write-Host '   until then the setup installs only from the voices\ folder beside it)' -ForegroundColor Yellow
}
if ($here) { Write-Host "  $($here.Count) of them copied to $(Join-Path $Out 'voices'), for the setup to install from there" }
if ($here) { Write-Host "  packed here: $($here.Name -join ', ')" }
if ($elsewhere) {
    # Normal: the models not packed here are the published ones (voices-info), already on the server.
    Write-Host "  $($elsewhere.Count) as published (voices-info), already on the server"
}
