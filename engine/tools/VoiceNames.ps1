<#
.SYNOPSIS
    The Windows voice a packed model gives, its speakers and what they are called, for
    tools\Build-Installer.ps1 (which writes the voices into the installer). Dot-source it, then call
    Get-ModelVoice and Get-SpokenVoices on a model's folder.

.DESCRIPTION
    Each model is one Windows voice, "Spoken Players Dwarf Male", "Spoken Players Leader Voices": WoW looks
    through every installed voice several times over for each one while it loads (a voice per speaker,
    300 of them, held it up for half a minute), so the speakers are not voices of their own. The addon
    picks one by a code at the start of the text ({sp:3}, the speaker's index), which the engine reads.

    The speakers are named for the game ("Spoken Dwarf Male", "Spoken Blood Elf Female"), each for itself
    where voice.json lists them ("Spoken Dwarf Male Player", "Spoken Dwarf Male Guard", "Spoken Draenei
    Female Tamuura"), else numbered ("Spoken Dwarf Male 2"): the names the addon knows them by (and
    each was a Windows voice of its own by, before).

    A folder named <race>_<sex>_<variant> is a second model for that race and sex, its voices named for
    the variant too: dwarf_male_quests gives "Spoken Dwarf Male Quests Guard" and so on.

    The folders leaders and murloc are no race, for the raid leader and raid warnings: their voices are
    "Spoken Leader Varian", "Spoken Leader Lich King", ... and "Spoken Murloc", "Spoken Murloc Baby".
#>

# The race models' folder names (the part before _male / _female) and what their voices are called.
$RaceNames = @{ human = 'Human'; dwarf = 'Dwarf'; nightelf = 'Night Elf'; gnome = 'Gnome'; skyborne = 'Skyborne';
                orc = 'Orc'; undead = 'Undead'; tauren = 'Tauren'; troll = 'Troll';
                bloodelf = 'Blood Elf'; voidelf = 'Void Elf'; nightborne = 'Nightborne'; darkiron = 'Dark Iron';
                earthen = 'Earthen'; mechagnome = 'Mechagnome'; zandalari = 'Zandalari'; kultiran = 'Kul Tiran';
                draenei = 'Draenei'; worgen = 'Worgen'; goblin = 'Goblin'; pandaren = 'Pandaren'; vulpera = 'Vulpera';
                dracthyr = 'Dracthyr' }

# Models that are no race (the raid leader's and raid warnings' voices), what their voices are called,
# and their speakers' names where the title-cased speaker name would not do.
$OtherModels = @{ leaders = 'Leader'; murloc = 'Murloc' }
$OtherSpeakers = @{ lichking = 'Lich King'; kiljaeden = "Kil'jaeden"; kelthuzad = "Kel'Thuzad"; guldan = "Gul'dan";
                   nzoth = "N'Zoth"; murloc = ''; babymurloc = 'Baby' }
$FemaleSpeakers = @('sylvanas', 'alexstrasza')

# What a speaker of a multi-speaker model is called in its voice's name: its label in voice.json
# (to_sherpa.py: "Tamuura", "Demon Hunter Player"), else from its speaker name, for packs made before
# the labels: "player" -> "Player", "spoken_guard" (a Spoken quest preset) -> "Guard", "spoken_extra1"
# (a Spoken voice without a preset name) -> "Quest Voice 1", "brann" (a retail NPC) -> "Brann".
function Get-SpeakerLabel([string]$speaker) {
    if (-not $speaker) { return $null }
    $text = (Get-Culture).TextInfo
    if ($speaker -match '^spoken_extra(\d+)$') { return "Quest Voice $($Matches[1])" }
    if ($speaker -match '^spoken_(.+)$') { return $text.ToTitleCase($Matches[1].Replace('_', ' ')) }
    return $text.ToTitleCase($speaker.Replace('_', ' '))
}

# A packed model's folder: finished when the build has written all three files.
function Test-PackFinished([string]$folder) {
    -not (@('model.onnx', 'tokens.txt', 'voice.json') | Where-Object { -not (Test-Path (Join-Path $folder $_)) })
}

# The Windows voice of the model in $folder: Name ("Spoken Players Orc Male", "Spoken Players Leader Voices",
# "Spoken Players Dwarf Male Quests") and Gender; nothing for a folder Get-SpokenVoices skips.
# The retrained leader model (2026-10-27, other speakers in another order) is "Spoken Players Leader
# Voices", not "Spoken Players Leaders" as the first one was: the addon knows a model's speakers only by
# its voice's name, so it can speak with either (and an addon older than this name takes it for a leader
# voice, "Spoken Leader Voices", never a player's).
$OtherVoiceNames = @{ leaders = 'Spoken Players Leader Voices'; murloc = 'Spoken Players Murlocs' }
function Get-ModelVoice([string]$folder) {
    $model = Split-Path $folder -Leaf
    if ($OtherVoiceNames.ContainsKey($model)) { return [pscustomobject]@{ Name = $OtherVoiceNames[$model]; Gender = 'Male' } }
    $race, $sex, $variant = $model -split '_', 3
    if (-not $RaceNames.ContainsKey($race) -or $sex -notin @('male', 'female')) { return }
    $text = (Get-Culture).TextInfo
    $name = "Spoken Players $($RaceNames[$race]) $($text.ToTitleCase($sex))"
    if ($variant) { $name += ' ' + $text.ToTitleCase($variant.Replace('_', ' ')) }
    [pscustomobject]@{ Name = $name; Gender = $text.ToTitleCase($sex) }
}

# The speakers of the model in $folder: Index (the speaker id in the model), Name, Gender and Speaker
# (voice.json's name for it, or $null). Nothing, with a warning, for a folder that is no
# <race>_<male|female>[_<variant>], leaders or murloc.
function Get-SpokenVoices([string]$folder) {
    $model = Split-Path $folder -Leaf
    $info = Get-Content (Join-Path $folder 'voice.json') -Raw | ConvertFrom-Json
    $other = $OtherModels.ContainsKey($model)
    if ($other) {
        $prefix, $gender, $variant = "Spoken $($OtherModels[$model])", 'Male', $null
    } else {
        $race, $sex, $variant = $model -split '_', 3
        if (-not $RaceNames.ContainsKey($race) -or $sex -notin @('male', 'female')) {
            Write-Warning "skipping ${model}: not <race>_<male|female>[_<variant>], leaders or murloc"
            return
        }
        $gender = (Get-Culture).TextInfo.ToTitleCase($sex)
        $prefix = "Spoken $($RaceNames[$race]) $gender"
        if ($variant) { $prefix += ' ' + (Get-Culture).TextInfo.ToTitleCase($variant.Replace('_', ' ')) }
    }
    $speakers = [Math]::Max(1, [int]$info.n_speakers)
    $who = @($info.speakers)
    $labels = @($info.labels)
    for ($speaker = 0; $speaker -lt $speakers; $speaker++) {
        $name = $prefix
        $id = $(if ($who.Count -eq $speakers) { $who[$speaker] })
        if ($other) {
            $gender = $(if ($id -in $FemaleSpeakers) { 'Female' } else { 'Male' })
            $label = $(if ($id -and $OtherSpeakers.ContainsKey($id)) { $OtherSpeakers[$id] } else { Get-SpeakerLabel $id })
        } elseif ($labels.Count -eq $speakers) {
            $label = $labels[$speaker]
        } else {
            $label = Get-SpeakerLabel $id
        }
        if ($label) { $name += " $label" }
        elseif ($speakers -gt 1 -and -not $id -or ($variant -and -not $id)) { $name += " $($speaker + 1)" }
        [pscustomobject]@{ Index = $speaker; Name = $name; Gender = $gender; Speaker = $id }
    }
}

