#!/usr/bin/env bash
# JSON -> binary -> JSON round trip through lvl_convert is stable, and PNG exports are written.
set -euo pipefail
CONVERT="$(realpath "$1")"
INPUT="$(realpath "$2")"
cd "${TEST_TMPDIR:-$(mktemp -d)}"
"${CONVERT}" "${INPUT}" a.json --memory=128
"${CONVERT}" a.json b.lvlb --memory=128
"${CONVERT}" b.lvlb c.json --memory=128
cmp a.json c.json || { echo "FAIL: JSON -> binary -> JSON changed the file"; diff a.json c.json | head; exit 1; }
"${CONVERT}" b.lvlb --info --memory=128 | tee info.txt
grep -q "layer: ground" info.txt
grep -q "1 portals, 1 markers, 1 path graphs (2 nodes, 1 edges)" info.txt
"${CONVERT}" b.lvlb --png map.png --ppc 4 --season winter --objects --memory=128
"${CONVERT}" b.lvlb --minimap mini.png --size 64 --memory=128
[[ -s map.png && -s mini.png ]] || { echo "FAIL: exports missing"; exit 1; }
# Errors are reported, not fatal
if "${CONVERT}" missing.json out.json --memory=128 2>err.txt; then echo "FAIL: missing input accepted"; exit 1; fi
grep -q "error:" err.txt
echo PASS
