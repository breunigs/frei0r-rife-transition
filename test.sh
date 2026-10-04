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
OUT_FPS=25
FRAMES=100
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

write_ratios() {
  local file="$1" pattern="$2"
  python3 - "${file}" "${pattern}" "${FRAMES}" <<'EOF'
import sys
path, pattern, count = sys.argv[1], sys.argv[2], int(sys.argv[3])
values = pattern.split(",")
with open(path, "w") as out:
    for i in range(count):
        out.write(values[i % len(values)] + "\n")
EOF
}

# The two inputs are the same source split in two, with one branch delayed by
# exactly one source frame, so [b] always carries frame i+1 where [a] carries
# frame i. <warp> is the identity (T) here, so output frames line up 1:1 with
# source frames and ratios index 0..99.
interp_graph() {
  local ratios="$1" debug="$2"
  local warp="T/TB" delay="PTS-$(python3 -c "print(1/${SRC_FPS})")/TB"
  echo "[0:v]split[x][y];\
[x]setpts='${warp}',fps=${OUT_FPS}:round=down[a];\
[y]setpts=${delay},setpts='${warp}',fps=${OUT_FPS}:round=down[b];\
[a][b]frei0r=filter_name=rife_transition:filter_params=0||0|${debug}|${ratios}|${OUT_FPS}[out]"
}

# Same graph with the frei0r stage removed. format=rgba matches what the frei0r
# stage forces, otherwise the hashes differ on pixel format alone.
reference_graph() {
  echo "[0:v]setpts='T/TB',fps=${OUT_FPS}:round=down,format=rgba[out]"
}

rife_count() {
  grep -o 'created [0-9]* RIFE frames' "$1" | tail -1 | grep -o '[0-9]*'
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

for spec in "r0:0.0" "r1:1.0" "r05:0.5" "rmix:0.0,0.5"; do
  write_ratios "${spec%%:*}.txt" "${spec#*:}"
done

ffmpeg_run ref.md5 "$(reference_graph)"
ffmpeg_run r0.md5 "$(interp_graph "${WORKDIR}/r0.txt" 0)"
ffmpeg_run r1.md5 "$(interp_graph "${WORKDIR}/r1.txt" 0)"
ffmpeg_run r05.md5 "$(interp_graph "${WORKDIR}/r05.txt" 1)"
ffmpeg_run rmix.md5 "$(interp_graph "${WORKDIR}/rmix.txt" 1)"
ffmpeg_run r05_again.md5 "$(interp_graph "${WORKDIR}/r05.txt" 1)"

# all ratios 0.0 => frame identical to the graph without the frei0r stage
if diff -q <(hashes ref.md5) <(hashes r0.md5) >/dev/null; then
  pass "ratios=0.0: frame identical to plain fps"
else
  fail "ratios=0.0: differs from plain fps"
fi

# all ratios 1.0 => exactly one frame ahead
if diff -q <(hashes r0.md5 | tail -n +2) <(hashes r1.md5 | head -n -1) >/dev/null; then
  pass "ratios=1.0: exactly one frame ahead of ratios=0.0"
else
  fail "ratios=1.0: not one frame ahead of ratios=0.0"
fi

# all ratios 0.5 => every frame interpolated, tally matches frame count
count=$(rife_count r05.md5.log)
out_frames=$(hashes r05.md5 | wc -l)
if [ "${count}" = "${out_frames}" ]; then
  pass "ratios=0.5: ${count} RIFE frames for ${out_frames} output frames"
else
  fail "ratios=0.5: ${count} RIFE frames, expected ${out_frames}"
fi
if diff -q <(hashes ref.md5) <(hashes r05.md5) >/dev/null; then
  fail "ratios=0.5: output equals the real frames, nothing was interpolated"
else
  pass "ratios=0.5: output differs from the real frames"
fi

# mixed ratios => no model call for the 0.0 entries
count=$(rife_count rmix.md5.log)
expected=$(grep -c '^0\.5$' rmix.txt)
if [ "${count}" = "${expected}" ]; then
  pass "mixed ratios: ${count} RIFE frames for ${expected} non-0/1 ratios"
else
  fail "mixed ratios: ${count} RIFE frames, expected ${expected}"
fi
if diff -q <(hashes rmix.md5 | awk 'NR%2==1') <(hashes ref.md5 | awk 'NR%2==1') >/dev/null; then
  pass "mixed ratios: ratio=0.0 frames are untouched real frames"
else
  fail "mixed ratios: ratio=0.0 frames were modified"
fi

# determinism, which a two pass encode depends on
if diff -q <(hashes r05.md5) <(hashes r05_again.md5) >/dev/null; then
  pass "determinism: repeated run is identical"
else
  fail "determinism: repeated run differs"
fi

# inline ratios, short list: exercises comma parsing and index clamping. Commas
# must be escaped in filtergraph syntax, which is why long lists use a file.
ffmpeg_run inline.md5 "$(interp_graph '0.0\,0.0\,0.0' 1)"
if diff -q <(hashes ref.md5) <(hashes inline.md5) >/dev/null; then
  pass "inline: comma separated ratios parse and clamp"
else
  fail "inline: comma separated ratios wrong"
fi
warnings=$(grep -c 'outside of the' inline.md5.log)
if [ "${warnings}" = "1" ]; then
  pass "inline: out of range index logged once"
else
  fail "inline: out of range index logged ${warnings} times, expected 1"
fi

echo
if [ "${failures}" = "0" ]; then
  echo "all checks passed"
else
  echo "${failures} check(s) failed"
fi
exit "${failures}"
