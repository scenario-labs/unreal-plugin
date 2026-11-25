#!/bin/bash
# Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.
#
# Compiles the Scenario plugin for an installed Unreal Engine and installs it
# into a project. Install-macOS.command at the top of the download starts it,
# so people can double-click it. It checks that Xcode is installed, asks for
# the project, finds the Unreal version that project uses, compiles the plugin
# with Unreal's RunUAT BuildPlugin and copies the result to
# <Project>/Plugins/Scenario. A plugin already there is moved to
# <Project>/Saved/Scenario/ first, never deleted. The full build log is written
# to <Project>/Saved/Scenario/ as well.
#
# Usage: install-scenario.sh [--project <file.uproject>] [--engine <Unreal folder>] [--yes]
#   --project  the project; without it, a Finder dialog asks for it
#   --engine   an Unreal folder, such as "/Users/Shared/Epic Games/UE_5.8";
#              without it, the Unreal version the project names, or a choice
#   --yes      ask nothing: when a choice is needed, fail instead
#
# Written for the bash 3.2 that ships with macOS; JSON is read with plutil.
set -uo pipefail

SUPPORTED="5.6 5.7 5.8"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd -P)"
PLUGIN="$ROOT/Plugins/Scenario/Scenario.uplugin"
STAMP="$(date +%Y%m%d-%H%M%S)"
LAUNCHER_LIST="${SCENARIO_LAUNCHER_LIST:-$HOME/Library/Application Support/Epic/UnrealEngineLauncher/LauncherInstalled.dat}"

project=""
engine_root=""
yes=0
while [ $# -gt 0 ]; do
  case "$1" in
    --project) project="${2:-}"; shift ;;
    --engine) engine_root="${2:-}"; shift ;;
    --yes) yes=1 ;;
    -h|--help) sed -n '4,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown option: $1 (see --help)" >&2; exit 2 ;;
  esac
  shift
done

bold() { printf '\n\033[1m%s\033[0m\n' "$1"; }
fail() {
  printf '\n\033[31m%s\033[0m\nNothing was installed.\n' "$1" >&2
  exit 1
}
# One value from a JSON file, or nothing.
json() { plutil -extract "$2" raw -o - "$1" 2>/dev/null; }
# "5.8" for an Unreal folder with RunUAT.sh, or nothing.
engine_version() {
  [ -x "$1/Engine/Build/BatchFiles/RunUAT.sh" ] || return 1
  local major minor
  major="$(json "$1/Engine/Build/Build.version" MajorVersion)" || return 1
  minor="$(json "$1/Engine/Build/Build.version" MinorVersion)" || return 1
  printf '%s.%s' "$major" "$minor"
}
supported() {
  case " $SUPPORTED " in *" $1 "*) return 0 ;; esac
  return 1
}
# One Unreal folder per line: the Epic Games Launcher's list, then the default folder.
find_engines() {
  local index=0 location
  while location="$(json "$LAUNCHER_LIST" "InstallationList.$index.InstallLocation")"; do
    printf '%s\n' "$location"
    index=$((index + 1))
  done
  for location in "/Users/Shared/Epic Games"/UE_*; do
    [ -d "$location" ] && printf '%s\n' "$location"
  done
}

echo "Scenario for Unreal Engine: compile and install"
[ -f "$PLUGIN" ] || fail "Plugins/Scenario/Scenario.uplugin is not next to this installer. Unzip the whole download, then run Install-macOS.command from the folder it made."
plugin_version="$(json "$PLUGIN" VersionName)"

# 1. Xcode, which Unreal compiles plugins with.
if ! xcode-select -p 2>/dev/null | grep -q '\.app/' || ! xcodebuild -version >/dev/null 2>&1; then
  printf '\n\033[31m%s\033[0m\n' "Unreal compiles plugins with Xcode, and it is not installed (the Command Line Tools alone are not enough)."
  echo "Install Xcode from the App Store, open it once to finish the setup, then run this installer again."
  if [ "$yes" = 0 ]; then
    open "macappstore://apps.apple.com/app/xcode/id497799835" 2>/dev/null || true
  fi
  fail "Install Xcode first."
fi

# 2. The project.
if [ -z "$project" ]; then
  [ "$yes" = 0 ] || fail "--project is needed with --yes."
  bold "Choose your Unreal project (.uproject) in the window that opens."
  project="$(osascript -e 'POSIX path of (choose file with prompt "Choose your Unreal project (.uproject)" of type {"uproject"})' 2>/dev/null)" \
    || fail "No project was chosen."
fi
case "$project" in *.uproject) ;; *) fail "This is not an Unreal project file (.uproject): $project" ;; esac
[ -f "$project" ] || fail "This is not an Unreal project file (.uproject): $project"
project_dir="$(cd "$(dirname "$project")" && pwd)"
project="$project_dir/$(basename "$project")"
destination="$project_dir/Plugins/Scenario"
case "$PLUGIN" in "$destination"/*) fail "This download is inside the project's Plugins/Scenario folder. Unzip it somewhere else, such as Downloads, and run the installer from there." ;; esac
echo "Project: $project"
if pgrep -fl UnrealEditor 2>/dev/null | grep -qF "$project"; then
  fail "Unreal Editor has this project open. Close it, then run the installer again."
fi

# 3. The Unreal version the project uses.
association="$(json "$project" EngineAssociation || true)"
if [ -n "$engine_root" ]; then
  engine_root="${engine_root%/}"
  version="$(engine_version "$engine_root")" || fail "No Unreal Engine in $engine_root (Engine/Build/BatchFiles/RunUAT.sh is missing)."
elif printf '%s' "$association" | grep -Eq '^\{?[0-9A-Fa-f-]{36}\}?$'; then
  # A source build, registered by its id.
  engine_root="$(sed -n "s/^$association=//p" "$HOME/Library/Application Support/Epic/UnrealEngine/Install.ini" 2>/dev/null | head -1)"
  engine_root="${engine_root%/}"
  version="$(engine_version "$engine_root")" \
    || fail "The project uses an Unreal source build ($association) that is not registered on this Mac. Run the installer with --engine <its folder>."
else
  matches=""
  count=0
  seen=""
  while IFS= read -r location; do
    location="${location%/}"
    case "$seen" in *"|$location|"*) continue ;; esac
    seen="$seen|$location|"
    found="$(engine_version "$location")" || continue
    if [ -n "$association" ]; then
      [ "$found" = "$association" ] || continue
    else
      supported "$found" || continue
    fi
    matches="$matches$found	$location
"
    count=$((count + 1))
  done <<EOF
$(find_engines)
EOF
  if [ "$count" = 0 ]; then
    if [ -n "$association" ]; then
      fail "The project uses Unreal $association, which is not installed here. Install it with the Epic Games Launcher, or run the installer with --engine <an Unreal folder>."
    fi
    fail "No Unreal ${SUPPORTED// /, } found. Install Unreal with the Epic Games Launcher, or run the installer with --engine <an Unreal folder>."
  fi
  sorted="$(printf '%s' "$matches" | sort -t '	' -k1,1 -r -V 2>/dev/null || printf '%s' "$matches" | sort -r)"
  choice=1
  if [ "$count" -gt 1 ] && [ "$yes" = 0 ]; then
    echo "Several Unreal versions are installed:"
    printf '%s\n' "$sorted" | awk -F '\t' '{ printf "  %d) Unreal %s  (%s)\n", NR, $1, $2 }'
    while :; do
      printf 'Type the number of the one your project uses (1-%s): ' "$count"
      read -r choice || fail "No Unreal version was chosen."
      case "$choice" in ''|*[!0-9]*) continue ;; esac
      [ "$choice" -ge 1 ] && [ "$choice" -le "$count" ] && break
    done
  fi
  line="$(printf '%s\n' "$sorted" | sed -n "${choice}p")"
  version="${line%%	*}"
  engine_root="${line#*	}"
fi
supported "$version" || fail "Scenario $plugin_version supports Unreal ${SUPPORTED// /, }; the project uses Unreal $version."
echo "Unreal $version: $engine_root"

# 4. Compile.
log_dir="$project_dir/Saved/Scenario"
mkdir -p "$log_dir" || fail "Could not create $log_dir."
log="$log_dir/Install-$STAMP.log"
# The temporary folder's real path: Unreal's build accelerator fails on the /var -> /private/var symlink.
stage="$(cd "${TMPDIR:-/tmp}" && pwd -P)/ScenarioBuild-$STAMP"
bold "Compiling Scenario $plugin_version for Unreal $version. This takes a few minutes."
echo "Full log: $log"
"$engine_root/Engine/Build/BatchFiles/RunUAT.sh" BuildPlugin -Plugin="$PLUGIN" -Package="$stage" -TargetPlatforms=Mac 2>&1 \
  | tee "$log" | grep --line-buffered -E '^\[[0-9]+/[0-9]+\]' | sed -u 's/^/  /'
status="${PIPESTATUS[0]}"
if [ "$status" != 0 ] || [ ! -f "$stage/Scenario.uplugin" ]; then
  errors="$(grep -E '(^|[[:space:]])(error|ERROR)[[:space:]:]|fatal error|^Unable to instantiate module' "$log" | head -10)"
  if [ -n "$errors" ]; then
    printf '\nThe first errors:\n'
    printf '%s\n' "$errors" | sed 's/^/  /'
  fi
  rm -rf "$stage"
  fail "The build failed (exit code $status). The full log is $log.
See https://github.com/scenario-labs/unreal-plugin/blob/main/docs/TROUBLESHOOTING.md"
fi

# 5. Install.
bold "Installing into $destination"
if [ -e "$destination" ]; then
  backup="$log_dir/PreviousPlugin-$STAMP"
  mv "$destination" "$backup" || fail "Could not move the current Plugins/Scenario folder aside. Close Unreal Editor and run the installer again."
  echo "The previous version is now in $backup"
fi
mkdir -p "$project_dir/Plugins" || fail "Could not create $project_dir/Plugins."
ditto "$stage" "$destination" || fail "Could not copy the plugin to $destination."
# Files unzipped from a download carry the quarantine flag, which can stop the editor from loading the plugin.
xattr -dr com.apple.quarantine "$destination" 2>/dev/null || true
rm -rf "$stage"

bold "Scenario $plugin_version is installed for Unreal $version."
echo "Open $project: the Scenario button is in the level editor toolbar."
