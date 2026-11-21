#!/usr/bin/env bash
# Up/down test: the editor starts, runs a scripted session (demo level, heightmap, water-shedding and
# WFC jobs, undo/redo, JSON + binary save, binary load, PNG + mini-preview + tileset/WFC/path
# exports) through the same code paths as the UI, and exits cleanly with no errors logged.
set -euo pipefail
BINARY="$(realpath "$1")"
# A software GL stack races inside Mesa itself, which would fail the thread-sanitizer build; the
# file says which report and why it is not ours. Ignored by builds that are not sanitized.
export TSAN_OPTIONS="suppressions=$(realpath "$2")${TSAN_OPTIONS:+:${TSAN_OPTIONS}}"
cd "${TEST_TMPDIR:-$(mktemp -d)}"
RUN=("${BINARY}" --smoke --frames=2000 --hidden --memory=256 --screenshot=final.png)
if [[ -z "${DISPLAY:-}" ]]; then
  RUN=(xvfb-run -a -s "-screen 0 1600x900x24" "${RUN[@]}")
fi
LOG="$(mktemp)"
if ! "${RUN[@]}" >"${LOG}" 2>&1; then
  cat "${LOG}"
  echo "FAIL: editor exited with an error"
  exit 1
fi
grep -v "(Debug:" "${LOG}" || true
fail() { echo "FAIL: $1"; exit 1; }
grep -q "Smoke test complete" "${LOG}" || fail "smoke session did not complete"
grep -q "Exited cleanly" "${LOG}" || fail "no clean exit"
# The session ends through "Save and quit": the last thing before exiting is a save
tail -n 20 "${LOG}" | grep -q "Saved level 'smoke.lvlb'" || fail "did not save before quitting"
if grep -q "(Error:" "${LOG}"; then fail "errors were logged"; fi
for f in lvl_demo_atlas.png smoke.json smoke.lvlb smoke.png smoke_preview.png smoke.tileset.json smoke.wfc.json \
         smoke.paths.lvlb smoke.style.json final.png; do
  [[ -s "${f}" ]] || fail "${f} was not written"
done
grep -q '"lvl-level"' smoke.json || fail "smoke.json is not a level"
if [[ -n "${TEST_UNDECLARED_OUTPUTS_DIR:-}" ]]; then
  cp final.png smoke.png smoke_preview.png "${TEST_UNDECLARED_OUTPUTS_DIR}/"
fi
echo "PASS"
