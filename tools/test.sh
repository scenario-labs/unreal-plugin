#!/usr/bin/env bash
# Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.
#
# Runs the plugin's automation tests headless in the host project and writes
# the report to tests/reports/<timestamp>/ (ignored by git). Exits non-zero
# when a test fails or no report is written.
#
# Usage: tools/test.sh [filter]       filter defaults to Scenario
#   tools/test.sh Scenario.Core       one group
# Build the editor target first: tools/build.sh --host
# Engine: $UE_ROOT, default /Users/Shared/Epic Games/UE_5.8.
# SCENARIO_TEST_PROJECT=<path to a .uproject> runs the tests in another project
# that loads this plugin, so they never share a job list with an editor that
# has the host project open.
# SCENARIO_TEST_ARGS="<editor arguments>" adds arguments to the editor, for
# example a renderer setting for one run:
#   SCENARIO_TEST_ARGS='-ini:Engine:[/Script/Engine.RendererSettings]:r.Substrate=True'
# SCENARIO_TEST_RENDER=1 renders offscreen (-RenderOffscreen) instead of
# -nullrhi, so tests that compile shaders or capture the viewport run fully.
# No window opens either way.
#
# The tests use fake Scenario replies (0 CU). SCENARIO_NO_SPEND=1 is set unless
# the caller sets it, so a call that reaches the real service by mistake cannot
# spend; the client tests set the switch themselves where they need it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UE_ROOT="${UE_ROOT:-/Users/Shared/Epic Games/UE_5.8}"
PROJECT="${SCENARIO_TEST_PROJECT:-$ROOT/ScenarioHost.uproject}"
filter="${1:-Scenario}"

case "$filter" in
  -h|--help) sed -n '4,24p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
esac

editor="$UE_ROOT/Engine/Binaries/Mac/UnrealEditor-Cmd"
if [ ! -x "$editor" ]; then
  editor="$UE_ROOT/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"
fi
if [ ! -x "$editor" ]; then
  echo "No Unreal Editor binary under UE_ROOT=$UE_ROOT. Install Unreal through the Epic Games Launcher or set UE_ROOT." >&2
  exit 1
fi

report="$ROOT/tests/reports/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$report"
export SCENARIO_NO_SPEND="${SCENARIO_NO_SPEND:-1}"

args=(
  "$PROJECT"
  "-ExecCmds=Automation RunTests $filter;Quit"
  -unattended -nosound -nosplash -nopause -stdout -log
  "-ReportExportPath=$report"
  "-abslog=$report/editor.log"
)
if [ "${SCENARIO_TEST_RENDER:-0}" = 1 ]; then
  args+=(-RenderOffscreen)
else
  args+=(-nullrhi)
fi
if [ -n "${SCENARIO_TEST_ARGS:-}" ]; then
  read -r -a extra <<< "$SCENARIO_TEST_ARGS"
  args+=("${extra[@]}")
fi
printf '+ %q' "$editor"
printf ' %q' "${args[@]}"
printf '\n'

status=0
"$editor" "${args[@]}" || status=$?

python3 - "$report/index.json" "$status" <<'PY'
import json, pathlib, sys

index = pathlib.Path(sys.argv[1])
status = int(sys.argv[2])
if not index.is_file():
    print(f"No report at {index} (editor exit code {status}); see editor.log next to it.")
    sys.exit(1)
data = json.loads(index.read_text(encoding="utf-8-sig"))
failed = [t for t in data.get("tests", []) if t.get("state") == "Fail"]
for test in failed:
    print(f"FAIL {test.get('fullTestPath', test.get('testDisplayName', '?'))}")
    for entry in test.get("entries", []):
        event = entry.get("event", {})
        if event.get("type") == "Error":
            print(f"     {event.get('message', '')}")
print(f"succeeded {data.get('succeeded', 0)}, with warnings {data.get('succeededWithWarnings', 0)}, "
      f"failed {data.get('failed', len(failed))}, not run {data.get('notRun', 0)}")
if not data.get("tests"):
    print("No test ran: check the filter and that the editor target is built (tools/build.sh --host).")
    sys.exit(1)
sys.exit(1 if failed or data.get("failed", 0) else 0)
PY
