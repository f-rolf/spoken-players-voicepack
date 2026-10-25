<#
.SYNOPSIS
    Tries a Spoken voice through Windows' own speech API (SAPI), without installing anything
    system-wide: the engine is registered for the current user only, and the voice token lives under
    HKCU\Software\SpokenVoiceTest. Writes WAV files next to this script.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\Test-Voice.ps1 -Voice dwarf_male
#>
param(
    [string]$Voice = "dwarf_male",
    [int]$SpeakerId = 0,
    [string]$VoicesFolder = 'voices-multi',
    [switch]$Cleanup
)

$ErrorActionPreference = 'Stop'
$Clsid = '{055bf9e8-68b3-4cb2-8090-50895f4bcc68}'
$Dist = Join-Path (Split-Path $PSScriptRoot -Parent) 'dist'
$Dll = Join-Path $Dist 'spoken_voice.dll'
$ClassKey = "HKCU:\Software\Classes\CLSID\$Clsid"
$TokenRoot = 'HKCU:\Software\SpokenVoiceTest\Tokens'

if ($Cleanup) {
    Remove-Item $ClassKey -Recurse -ErrorAction SilentlyContinue
    Remove-Item 'HKCU:\Software\SpokenVoiceTest' -Recurse -ErrorAction SilentlyContinue
    return
}

New-Item "$ClassKey\InprocServer32" -Force | Out-Null
Set-ItemProperty $ClassKey -Name '(default)' -Value 'Spoken Voice Engine'
Set-ItemProperty "$ClassKey\InprocServer32" -Name '(default)' -Value $Dll
Set-ItemProperty "$ClassKey\InprocServer32" -Name 'ThreadingModel' -Value 'Both'

$token = "$TokenRoot\$Voice"
New-Item "$token\Attributes" -Force | Out-Null
Set-ItemProperty $token -Name '(default)' -Value "Spoken Test $Voice"
Set-ItemProperty $token -Name 'CLSID' -Value $Clsid
Set-ItemProperty $token -Name 'ModelDir' -Value (Join-Path $Dist "$VoicesFolder\$Voice")
Set-ItemProperty $token -Name 'DataDir' -Value (Join-Path $Dist 'espeak-ng-data')
Set-ItemProperty $token -Name 'SpeakerId' -Value $SpeakerId -Type DWord
Set-ItemProperty "$token\Attributes" -Name 'Name' -Value "Spoken Test $Voice"
Set-ItemProperty "$token\Attributes" -Name 'Language' -Value '409'

$objectToken = New-Object -ComObject SAPI.SpObjectToken
$objectToken.SetId(($token -replace '^HKCU:', 'HKEY_CURRENT_USER'))
$sapi = New-Object -ComObject SAPI.SpVoice
$sapi.Voice = $objectToken

$tests = [ordered]@{
    'plain' = 'Pulling in three. Watch the patrol on the left, it has a skull!'
    'pitch-up' = '<pitch middle="2">Who wants the ring? I will trade it to you.</pitch>'
    'pitch-down' = '<pitch middle="-2">Who wants the ring? I will trade it to you.</pitch>'
    'fast' = '<rate speed="4">Thanks for the group, everyone. See you tomorrow.</rate>'
}
foreach ($name in $tests.Keys) {
    $path = Join-Path $PSScriptRoot "test_${Voice}_${VoicesFolder}_${SpeakerId}_$name.wav"
    $stream = New-Object -ComObject SAPI.SpFileStream
    $stream.Open($path, 3)  # SSFMCreateForWrite
    $sapi.AudioOutputStream = $stream
    $started = Get-Date
    [void]$sapi.Speak($tests[$name], 8)  # SVSFIsXML
    $stream.Close()
    $ms = [int]((Get-Date) - $started).TotalMilliseconds
    $bytes = (Get-Item $path).Length
    Write-Host ("{0,-11} {1,6:N1} s of audio in {2,5} ms  -> {3}" -f $name, (($bytes - 44) / 44100.0), $ms, (Split-Path $path -Leaf))
}
