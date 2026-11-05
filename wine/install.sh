#!/usr/bin/env bash
# Sets the Spoken Players Voicepack up for World of Warcraft on Linux: finds the Wine or Proton prefix
# WoW is installed in, and runs SpokenPlayersVoicepack-Wine.exe inside it with the Proton or Wine
# that prefix was made with (so the prefix is not upgraded to another one).
#
#   ./install.sh                              finds WoW; the voicepack's setup opens: choose the game
#   ./install.sh --prefix DIR                 that prefix (the folder with drive_c in it)
#   ./install.sh --game forever|retail|all    no windows: that game's voices, silently
#   ./install.sh --test                       says the test line again
#   ./install.sh --uninstall                  takes SapiFix out
#   ./install.sh ... -- /OPTION               passes options on to the .exe (its /? lists them)
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
prefix=
args=()

while (( $# )); do
  case $1 in
    --prefix) prefix=$2; shift ;;
    --game) args+=(/quiet "/game=$2"); shift ;;
    --test) args+=(/test) ;;
    --uninstall) args+=(/uninstall) ;;
    -h|--help) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    --) shift; args+=("$@"); break ;;
    *) echo "Unknown option: $1 (see --help)" >&2; exit 2 ;;
  esac
  shift
done

# The .exe: beside this script (the zip), or built in the repository (out/wine).
exe=
for candidate in "$here/SpokenPlayersVoicepack-Wine.exe" "$here/../out/wine/SpokenPlayersVoicepack-Wine.exe"; do
  if [[ -f $candidate ]]; then exe=$(realpath "$candidate"); break; fi
done
if [[ -z $exe && -x $here/build.sh ]]; then
  "$here/build.sh"
  exe=$(realpath "$here/../out/wine/SpokenPlayersVoicepack-Wine.exe")
fi
[[ -n $exe ]] || { echo "SpokenPlayersVoicepack-Wine.exe is not beside this script." >&2; exit 1; }

# The prefixes WoW (or Battle.net) is installed in, where launchers keep them.
find_prefixes() {
  local roots=(
    "$HOME/Games" "$HOME/Faugus" "$HOME/.wine" "$HOME/Games/Heroic/Prefixes"
    "$HOME/.local/share/Steam/steamapps/compatdata" "$HOME/.steam/steam/steamapps/compatdata"
    "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/compatdata"
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
Say where it is:  ./install.sh --prefix /path/to/the/prefix   (the folder with drive_c in it)
Or run this file inside WoW's prefix with your launcher ("Run EXE inside Wine prefix" and the like):
  $exe
EOF
    exit 1
  elif (( ${#found[@]} == 1 )); then
    prefix=${found[0]}
  else
    echo "World of Warcraft is in more than one prefix:"
    for i in "${!found[@]}"; do echo "  $((i + 1))) ${found[$i]}"; done
    read -rp "Which one? " choice </dev/tty
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
mkdir -p "$inside"
cp "$exe" "$inside/"
setup_beside="$(dirname "$exe")/SpokenPlayersVoicepack-Setup.exe"
[[ -f $setup_beside ]] && cp "$setup_beside" "$inside/"
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
  proton=$(sed -n 2p "$compat/config_info")
  proton=${proton%/}; proton=${proton%/share/fonts}; proton=${proton%/files}; proton=${proton%/dist}
  [[ -x $proton/proton ]] || { echo "The Proton this prefix was made with is gone ($proton). Start WoW once, then run this again." >&2; exit 1; }
  how="Proton ($(basename "$proton"))"
  steam=$HOME/.local/share/Steam
  [[ $compat == */steamapps/compatdata/* ]] && steam=${compat%/steamapps/compatdata/*}
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
[[ -n $user_dir ]] || user_dir=$(find "$prefix/drive_c/users" -mindepth 1 -maxdepth 1 -type d ! -name Public | head -1)
setup_log="$user_dir/AppData/Local/SapiFix/setup.log"
mkdir -p "$(dirname "$setup_log")"
touch "$setup_log"

echo "World of Warcraft's prefix: $prefix"
echo "Running SpokenPlayersVoicepack-Wine.exe in it with $how."
[[ " ${args[*]} " == *" /quiet "* ]] || echo "Windows open next: follow them."
echo
set +e
"${cmd[@]}" "${args[@]}" >"$log" 2>&1 &
run=$!
tail -n0 -F --pid="$run" "$setup_log" 2>/dev/null | sed -u 's/^[0-9-]* [0-9:]* //'
wait "$run"
status=$?
set -e
if (( status )); then
  echo
  echo "It did not finish (exit code $status). Its log: $setup_log"
  echo "What Wine printed: $log"
fi
exit "$status"
