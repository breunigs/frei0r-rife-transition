#!/bin/bash

# Verifies the rife_transition plugin.
#
# Usage:
#   ./test.sh            run all checks
#   ./test.sh --record   only (re-)record the baselines
#
# Checks that compare against a baseline need it recorded with the previous
# build first:
#
#   git stash && ./install.sh && ./test.sh --record
#   git stash pop && ./install.sh && ./test.sh
#
# Baselines live in the work dir, not in git, because RIFE output depends on the
# GPU and driver of the machine it was rendered on.

set -uo pipefail

WORKDIR="${WORKDIR:-/tmp/rife_transition_test}"
SRC_FPS=25
BASELINE="${WORKDIR}/baseline_transition.md5"

mkdir -p "${WORKDIR}"
cd "${WORKDIR}"

failures=0
pass() { echo "PASS  $*"; }
fail() {
  echo "FAIL  $*"
  failures=$((failures + 1))
}

# strips the framemd5 header and the per-frame metadata, leaving only hashes
hashes() { grep -v '^#' "$1" | awk -F', ' '{print $NF}'; }

ffmpeg_run() {
  local out="$1" graph="$2"
  ffmpeg -nostdin -y -loglevel info -i src.mkv -filter_complex "${graph}" -map '[out]' -f framemd5 "${out}" \
    2>"${out}.log"
}

# A counter video, so frame identity is visible when inspecting output by eye.
make_source() {
  local src="testsrc2=size=640x360:rate=${SRC_FPS}:duration=4"
  src+=",drawtext=text='%{n}':fontsize=72:fontcolor=white:x=20:y=20"
  ffmpeg -nostdin -y -loglevel error -f lavfi -i "${src}" -c:v ffv1 src.mkv
}

transition_graph() {
  echo "[0:v]split[x][y];[x]null[a];[y]null[b];\
[a][b]frei0r=filter_name=rife_transition:filter_params=0.267||0[out]"
}

make_source

if [ "${1:-}" = "--record" ]; then
  echo "recording transition baseline"
  ffmpeg_run "${BASELINE}" "$(transition_graph)"
  echo "wrote ${BASELINE}"
  exit 0
fi

# a transition must keep rendering identically
if [ -f "${BASELINE}" ]; then
  ffmpeg_run transition.md5 "$(transition_graph)"
  if diff -q <(hashes "${BASELINE}") <(hashes transition.md5) >/dev/null; then
    pass "transition: output unchanged"
  else
    fail "transition: output changed"
  fi
else
  echo "SKIP  transition: no baseline, run ./test.sh --record against the old plugin first"
fi

echo
if [ "${failures}" = "0" ]; then
  echo "all checks passed"
else
  echo "${failures} check(s) failed"
fi
exit "${failures}"
