#!/usr/bin/env bash
# Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.
#
# Packages the plugin with RunUAT BuildPlugin into
# Build/Scenario-<version>-UE<major.minor>/, and with --host also builds the
# host project's editor target (ScenarioHostEditor), which tools/test.sh runs.
#
# Usage: tools/build.sh [--host] [--platforms Mac] [-- extra RunUAT arguments]
# Engine: $UE_ROOT, default /Users/Shared/Epic Games/UE_5.8 (launcher install).
#
# Unverified until Unreal 5.8 is installed on the development Mac: the
# commands follow Epic's documented RunUAT BuildPlugin and Build.sh usage.
# An existing package folder is moved to Build/archive/, never deleted.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UE_ROOT="${UE_ROOT:-/Users/Shared/Epic Games/UE_5.8}"
PLUGIN="$ROOT/Plugins/Scenario/Scenario.uplugin"
PROJECT="$ROOT/ScenarioHost.uproject"
RUNUAT="$UE_ROOT/Engine/Build/BatchFiles/RunUAT.sh"
BUILD_SH="$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh"

host=0
platforms="Mac"
extra=()
while [ $# -gt 0 ]; do
  case "$1" in
    --host) host=1 ;;
    --platforms) platforms="${2:?--platforms needs a value, for example Mac}"; shift ;;
    --) shift; extra=("$@"); break ;;
    -h|--help) sed -n '4,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown option: $1 (see --help)" >&2; exit 2 ;;
  esac
  shift
done

if [ ! -x "$RUNUAT" ]; then
  echo "RunUAT.sh not found under UE_ROOT=$UE_ROOT. Install Unreal through the Epic Games Launcher or set UE_ROOT." >&2
  exit 1
fi

version="$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1], encoding="utf-8-sig"))["VersionName"])' "$PLUGIN")"
engine="$(python3 -c 'import json, sys; d = json.load(open(sys.argv[1], encoding="utf-8-sig")); print("%s.%s" % (d["MajorVersion"], d["MinorVersion"]))' "$UE_ROOT/Engine/Build/Build.version")"
out="$ROOT/Build/Scenario-$version-UE$engine"

if [ -e "$out" ]; then
  mkdir -p "$ROOT/Build/archive"
  moved="$ROOT/Build/archive/$(basename "$out")-$(date +%Y%m%d-%H%M%S)"
  echo "Moving the previous package to $moved"
  mv "$out" "$moved"
fi

run() {
  printf '+'
  printf ' %q' "$@"
  printf '\n'
  "$@"
}

if [ "$host" = 1 ]; then
  run "$BUILD_SH" ScenarioHostEditor Mac Development -Project="$PROJECT" -WaitMutex
fi

run "$RUNUAT" BuildPlugin -Plugin="$PLUGIN" -Package="$out" -TargetPlatforms="$platforms" ${extra[@]+"${extra[@]}"}

echo "Plugin $version for Unreal $engine packaged in $out"
