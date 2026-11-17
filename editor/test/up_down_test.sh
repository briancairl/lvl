#!/usr/bin/env bash
# Up/down test: the editor starts, creates the demo level, renders frames and exits cleanly.
set -euo pipefail
BINARY="$(realpath "$1")"
cd "${TEST_TMPDIR:-$(mktemp -d)}"
RUN=("${BINARY}" --demo --frames=300 --hidden --memory=256)
if [[ -z "${DISPLAY:-}" ]]; then
  RUN=(xvfb-run -a -s "-screen 0 1600x900x24" "${RUN[@]}")
fi
LOG="$(mktemp)"
if ! "${RUN[@]}" >"${LOG}" 2>&1; then
  cat "${LOG}"
  echo "FAIL: editor exited with an error"
  exit 1
fi
cat "${LOG}"
grep -q "Created demo level" "${LOG}" || { echo "FAIL: demo level not created"; exit 1; }
grep -q "Exited cleanly after 300 frames" "${LOG}" || { echo "FAIL: no clean exit"; exit 1; }
[[ -f lvl_demo_atlas.png ]] || { echo "FAIL: demo atlas not written"; exit 1; }
echo "PASS"
