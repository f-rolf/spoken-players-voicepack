#!/usr/bin/env bash
# Sets the Spoken Players Voicepack up for World of Warcraft on Linux: finds the Wine or Proton prefix
# WoW is installed in, and runs SpokenPlayersVoicepack-Wine.exe inside it with the Proton or Wine
# that prefix was made with (so the prefix is not upgraded to another one). Run it again to update:
# only the voices that changed are downloaded.
#
#   bash install.sh                           asks which game you play, then installs its voices
#   bash install.sh --game forever|retail|all that game's voices, without asking
#   bash install.sh --wizard                  the voicepack's setup window instead, races one by one
#   bash install.sh --prefix DIR              that prefix (the folder with drive_c in it)
#   bash install.sh --test                    checks again that the voices speak (silently)
#   bash install.sh --uninstall [--yes]       takes it all out again: voices, engine, SapiFix
#   bash install.sh --dry-run ...             says what it would run, and runs nothing
#   bash install.sh ... -- /OPTION            passes options on to the .exe
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
prefix=
game=
mode=install
yes=
dry_run=
extra=()

while (( $# )); do
  case $1 in
    --prefix) prefix=${2:?--prefix needs a folder}; shift ;;
    --game)
      game=${2:?--game needs forever, retail or all}; shift
      [[ $game == forever || $game == retail || $game == all ]] || { echo "--game is forever, retail or all, not $game" >&2; exit 2; } ;;
    --wizard) mode=wizard ;;
    --test) mode=test ;;
    --uninstall) mode=uninstall ;;
    --yes|-y) yes=1 ;;
    --dry-run) dry_run=1 ;;
    -h|--help) awk 'NR == 1 { next } !/^#/ { exit } { sub(/^# ?/, ""); print }' "$0"; exit 0 ;;
    --) shift; extra+=("$@"); break ;;
    *) echo "Unknown option: $1 (see --help)" >&2; exit 2 ;;
  esac
  shift
done

# A question for the player, on the terminal even when stdin is not one.
ask() {
  local answer
  [[ -r /dev/tty ]] || return 1
  read -rp "$1" answer </dev/tty || return 1
  printf '%s' "$answer"
}

# The .exe: beside this script (the zip; wine\Build-Wine.ps1 builds it on Windows).
exe=
[[ -f $here/SpokenPlayersVoicepack-Wine.exe ]] && exe=$(realpath "$here/SpokenPlayersVoicepack-Wine.exe")
if [[ -z $exe ]]; then
  if [[ -f $here/../wine/Build-Wine.ps1 ]]; then
    echo "This is the repository's copy of install.sh: the .exe it runs is built on Windows by" >&2
    echo "wine\Build-Wine.ps1, into out\SpokenPlayersVoicepack-Linux.zip: unzip that and run its install.sh." >&2
  else
    echo "SpokenPlayersVoicepack-Wine.exe is not beside this script: unzip all of" >&2
    echo "SpokenPlayersVoicepack-Linux.zip and run the install.sh in it (from any folder)." >&2
  fi
  exit 1
fi

# The prefixes WoW (or Battle.net) is installed in, where launchers keep them.
find_prefixes() {
  local roots=(
    "$HOME/Games" "$HOME/Faugus" "$HOME/.wine" "$HOME/Games/Heroic/Prefixes"
    "$HOME/.local/share/Steam/steamapps/compatdata" "$HOME/.steam/steam/steamapps/compatdata"
    "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/compatdata"
    "$HOME/.var/app/com.valvesoftware.Steam/data/Steam/steamapps/compatdata"
    "$HOME/snap/steam/common/.local/share/Steam/steamapps/compatdata"
    "$HOME/.local/share/lutris" "$HOME/.var/app/net.lutris.Lutris/data/lutris"
    "$HOME/.local/share/bottles/bottles" "$HOME/.var/app/com.usebottles.bottles/data/bottles/bottles"
  )
  local root drive_c
  for root in "${roots[@]}"; do
    [[ -d $root ]] || continue
    while IFS= read -r drive_c; do
      if [[ -d "$drive_c/Program Files (x86)/World of Warcraft" || -d "$drive_c/Program Files (x86)/Battle.net" ]]; then
        realpath "$(dirname "$drive_c")"
      fi
    done < <(find -H "$root" -maxdepth 4 -type d -name drive_c -prune 2>/dev/null)
  done | sort -u
}

if [[ -z $prefix ]]; then
  mapfile -t found < <(find_prefixes)
  if (( ${#found[@]} == 0 )); then
    cat >&2 <<EOF
Could not find World of Warcraft in a Wine or Proton prefix.
Say where it is:  bash install.sh --prefix /path/to/the/prefix   (the folder with drive_c in it)
Or run this file inside WoW's prefix with your launcher ("Run EXE inside Wine prefix" and the like):
  $exe
EOF
    exit 1
  elif (( ${#found[@]} == 1 )); then
    prefix=${found[0]}
  else
    echo "World of Warcraft is in more than one prefix:"
    for i in "${!found[@]}"; do echo "  $((i + 1))) ${found[$i]}"; done
    choice=$(ask "Which one? ") || { echo "Say which with --prefix DIR." >&2; exit 2; }
    [[ $choice =~ ^[0-9]+$ ]] && (( choice >= 1 && choice <= ${#found[@]} )) || { echo "No such one." >&2; exit 2; }
    prefix=${found[$((choice - 1))]}
  fi
fi
prefix=$(realpath "$prefix")
[[ -d $prefix/drive_c ]] || { echo "$prefix is not a Wine prefix (it has no drive_c)." >&2; exit 1; }

# How to run a program in it. Proton writes config_info beside the prefix it made (umu-launcher,
# Lutris, Heroic, Faugus: in the prefix; Steam: in compatdata/<id>, the prefix being its pfx),
# naming its own files; a plain Wine prefix is run with Lutris' Wine for it, else the system's.
log="$HOME/.cache/spoken-players-voicepack/install.log"
mkdir -p "$(dirname "$log")"
cmd=()
compat=

# Run from inside the prefix: Proton takes a program outside it for a game on a drive of its own,
# waits for that "game" long after it ends, and passes on none of its output.
inside="$prefix/drive_c/windows/temp/SpokenPlayersVoicepack"
if [[ -z $dry_run ]]; then
  mkdir -p "$inside"
  cp "$exe" "$inside/"
  setup_beside="$(dirname "$exe")/SpokenPlayersVoicepack-Setup.exe"
  [[ -f $setup_beside ]] && cp "$setup_beside" "$inside/"
fi
win_exe='C:\windows\temp\SpokenPlayersVoicepack\SpokenPlayersVoicepack-Wine.exe'
if [[ -f $prefix/config_info ]]; then
  compat=$prefix
elif [[ $(basename "$prefix") == pfx && -f $prefix/../config_info ]]; then
  compat=$(dirname "$prefix")
fi

if [[ $prefix == */bottles/* ]]; then
  cat <<EOF
This prefix is a Bottles bottle: run this file in it with Bottles' "Run Executable..." button:
  $exe
EOF
  exit 0
elif [[ -n $compat ]]; then
  steam=$HOME/.local/share/Steam
  [[ $compat == */steamapps/compatdata/* ]] && steam=${compat%/steamapps/compatdata/*}
  made_with=$(sed -n 2p "$compat/config_info")
  made_with=${made_with%/}; made_with=${made_with%/share/fonts}; made_with=${made_with%/files}; made_with=${made_with%/dist}
  # As the Proton saw itself: Flatpak's Steam sees its home where this script sees ~/.var/app/...,
  # so a Proton of Steam's is looked for in the Steam folder the prefix is in too.
  proton=
  for candidate in "$made_with" \
                   "$steam/steamapps/common/${made_with#*/steamapps/common/}" \
                   "$steam/compatibilitytools.d/${made_with#*/compatibilitytools.d/}"; do
    if [[ -x $candidate/proton ]]; then proton=$candidate; break; fi
  done
  [[ -n $proton ]] || { echo "The Proton this prefix was made with is gone ($made_with). Start WoW once, then run this again." >&2; exit 1; }
  how="Proton ($(basename "$proton"))"
  if command -v umu-run >/dev/null; then
    cmd=(env WINEPREFIX="$compat" PROTONPATH="$proton" GAMEID=umu-spokenplayers PROTON_VERB=run umu-run "$win_exe")
    how+=" through umu-launcher"
  elif [[ $compat == */steamapps/compatdata/* ]] && command -v protontricks-launch >/dev/null; then
    cmd=(protontricks-launch --appid "$(basename "$compat")" "$inside/SpokenPlayersVoicepack-Wine.exe")
    how+=" through protontricks"
  else
    cmd=(env STEAM_COMPAT_DATA_PATH="$compat" STEAM_COMPAT_CLIENT_INSTALL_PATH="$steam" "$proton/proton" run "$win_exe")
  fi
else
  wine=
  for config in "$HOME"/.config/lutris/games/*.yml "$HOME"/.local/share/lutris/games/*.yml \
                "$HOME"/.var/app/net.lutris.Lutris/config/lutris/games/*.yml; do
    [[ -f $config ]] || continue
    configured=$(sed -n 's/^ *prefix: *//p' "$config" | head -1)
    [[ -n $configured && $(realpath -m "${configured/#\~/$HOME}") == "$prefix" ]] || continue
    version=$(awk '/^wine:/{w=1; next} /^[^ ]/{w=0} w && /version:/{print $2; exit}' "$config")
    for runners in "$HOME/.local/share/lutris/runners/wine" "$HOME/.var/app/net.lutris.Lutris/data/lutris/runners/wine"; do
      [[ -n $version && -x $runners/$version/bin/wine ]] && wine=$runners/$version/bin/wine
    done
  done
  [[ -n $wine ]] || wine=$(command -v wine || true)
  [[ -n $wine ]] || { echo "No Wine to run it with: install wine, or run this file in the prefix with your launcher: $exe" >&2; exit 1; }
  how="Wine ($("$wine" --version 2>/dev/null || echo "$wine"))"
  cmd=(env WINEPREFIX="$prefix" "$wine" "$win_exe")
fi

# What it does shows from its log as it goes: not every Proton passes a program's output on.
user_dir=
for user in steamuser "$USER"; do
  if [[ -d $prefix/drive_c/users/$user ]]; then user_dir=$prefix/drive_c/users/$user; break; fi
done
[[ -n $user_dir ]] || user_dir=$(find "$prefix/drive_c/users" -mindepth 1 -maxdepth 1 -type d ! -name Public 2>/dev/null | head -1 || true)
[[ -n $user_dir ]] || { echo "$prefix has no user folder (drive_c/users): start WoW once with it, then run this again." >&2; exit 1; }
setup_log="$user_dir/AppData/Local/SapiFix/setup.log"
pack_log="$user_dir/AppData/Local/SapiFix/voicepack-setup.log"

# The game chosen last time in this prefix (the voicepack's setup keeps it in the registry), if any.
last_game() {
  [[ -f $prefix/system.reg ]] || return 0
  awk '/^\[/ { inside = tolower($0) ~ /^\[software\\\\spoken players\]/ }
       inside && /^"Game"="/ { sub(/^"Game"="/, ""); sub(/".*/, ""); print; exit }' "$prefix/system.reg"
}

game_title() {
  case $1 in retail) echo 'Retail' ;; forever) echo 'WoW Forever' ;; all) echo 'Both games' ;; *) echo "$1" ;; esac
}

echo "World of Warcraft's prefix: $prefix"
echo "Run with $how."
echo

case $mode in
  install)
    if [[ -z $game ]]; then
      last=$(last_game)
      default=; [[ $last == retail ]] && default=1; [[ $last == forever ]] && default=2; [[ $last == all ]] && default=3
      echo 'Which World of Warcraft do you play? Only its voices are downloaded (from Hugging Face).'
      echo '  1) Retail          every race but WoW Forever'\''s Skyborne, and the raid voices'
      echo '  2) WoW Forever     its nine races, and the raid voices'
      echo '  3) Both'
      prompt='Choose 1, 2 or 3: '
      [[ -n $default ]] && prompt="Choose 1, 2 or 3 [Enter: $default, $(game_title "$last") as last time]: "
      choice=$(ask "$prompt") || { echo "No terminal to ask on: say --game forever, --game retail or --game all." >&2; exit 2; }
      [[ -z $choice && -n $default ]] && choice=$default
      case $choice in 1) game=retail ;; 2) game=forever ;; 3) game=all ;; *) echo "No such choice: $choice" >&2; exit 2 ;; esac
      echo
    fi
    echo "Installing the voices for $(game_title "$game"). The first time downloads them (about 70 MB each); after that, only those that changed."
    args=(/quiet "/game=$game") ;;
  wizard)
    echo "The voicepack's setup opens next, in a window: follow it."
    args=() ;;
  test)
    args=(/quiet /test) ;;
  uninstall)
    if [[ -z $yes ]]; then
      echo "This takes the Spoken Players voices out of this prefix: the voices, their engine, SapiFix, and WoW's"
      echo "setting for them. WoW then speaks with its own two voices again. The addon stays."
      answer=$(ask 'Go on? [y/N] ') || { echo "No terminal to ask on: say --yes." >&2; exit 2; }
      [[ $answer == [yY]* ]] || { echo "Nothing changed."; exit 0; }
    fi
    args=(/quiet /uninstall) ;;
esac
args+=("${extra[@]}")

if [[ -n $dry_run ]]; then
  printf 'Would run:'; printf ' %q' "${cmd[@]}" "${args[@]}"; echo
  exit 0
fi

# Each run's logs from their start, so that tail misses none of their first lines; the run before's
# stays as setup.old.log.
mkdir -p "$(dirname "$setup_log")"
[[ -f $setup_log ]] && mv -f "$setup_log" "${setup_log%.log}.old.log"
rm -f "$pack_log"
touch "$setup_log"
echo
set +e
"${cmd[@]}" "${args[@]}" >"$log" 2>&1 &
run=$!
# What it does, from its log as it goes (not every Proton passes a program's output on), and which
# voice the voicepack's setup is downloading, from its own log.
tail -q -c +1 -F --pid="$run" "$setup_log" "$pack_log" 2>/dev/null | awk '
  /Downloading https:\/\// { n = split($0, part, "/"); print "  downloading the " part[n - 1] " voice ..."; fflush(); next }
  /[0-9]+-[0-9]+-[0-9]+ [0-9:]+\.[0-9]+ / { next }  # the rest of the Inno Setup log, its first line after a BOM too
  { sub(/^[0-9]+-[0-9]+-[0-9]+ [0-9:]+ /, ""); print; fflush() }'
wait "$run"
status=$?
set -e
if (( status )); then
  echo
  echo "It did not finish (exit code $status). Its log: $setup_log"
  echo "What Wine printed: $log"
  exit "$status"
fi
exit 0
