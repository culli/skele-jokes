#!/bin/bash
# Measure loudness and, when needed, make a louder MP3 for the DY-SV5W.
# Requires ffmpeg (with libmp3lame) and Python 3. Works with macOS Bash 3.2.
set -euo pipefail

TARGET_LUFS=-16
TRUE_PEAK_DB=-1.5
LOUDNESS_RANGE=11
TOLERANCE_LU=0.5

usage() {
  cat <<'EOF'
Usage: normalize-audio.sh INPUT_FILE [OUTPUT.mp3]

Measure the first audio stream and increase its loudness if it is below
-16.5 LUFS. Target: -16 LUFS, with a -1.5 dBTP limiter ceiling before MP3
encoding. Output: stereo, 44.1 kHz, 192 kbps MP3.

Default output: INPUT_FILE's folder and base name, ending in -louder.mp3.
The original and any existing output file are never overwritten.
Files already loud enough, silent, or unmeasurable produce no new file.

Examples:
  ./normalize-audio.sh "/path/to/00001.mp3"
  ./normalize-audio.sh "/path/to/voice memo.mp3" "/path/to/louder/00001.mp3"

The optional output folder must already exist. Use -- before a filename
starting with a dash. Edit the constants near the top to change targets.
EOF
}

die() { printf 'Error: %s\n' "$*" >&2; exit 1; }

case "${1:-}" in
  -h|--help) usage; exit 0 ;;
  --) shift ;;
esac
if [[ $# -lt 1 || $# -gt 2 ]]; then usage >&2; exit 2; fi
for tool in ffmpeg python3; do
  command -v "$tool" >/dev/null 2>&1 || die "Install $tool first."
done

input=$1
case "$input" in /*) ;; *) input="$PWD/$input" ;; esac
[[ -f "$input" && -r "$input" ]] || die "Cannot read input: $input"
filename=${input##*/}
stem=${filename%.*}
[[ -n "$stem" ]] || stem=$filename
output=${2:-"${input%/*}/${stem}-louder.mp3"}
case "$output" in /*) ;; *) output="$PWD/$output" ;; esac
case "$output" in *.[mM][pP]3) ;; *) die "Output must end in .mp3" ;; esac

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/normalize-audio.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
analysis_filter="loudnorm=I=$TARGET_LUFS:TP=$TRUE_PEAK_DB:LRA=$LOUDNESS_RANGE:print_format=json"

# Keep diagnostics separate, then extract loudnorm's JSON without evaluating
# shell commands. Non-finite readings cannot be used for normalization.
measure() {
  local file=$1 tag=$2
  if ! ffmpeg -nostdin -hide_banner -nostats -i "$file" \
      -map 0:a:0 -vn -sn -dn -af "$analysis_filter" -f null - \
      >/dev/null 2>"$work_dir/$tag.log"; then
    cat "$work_dir/$tag.log" >&2
    die "Could not measure: $file"
  fi
  python3 - "$work_dir/$tag.log" "$TARGET_LUFS" "$TOLERANCE_LU" \
      >"$work_dir/$tag.values" <<'PY'
import json
import math
import re
import sys

with open(sys.argv[1], encoding="utf-8", errors="replace") as log:
    blocks = re.findall(r'\{[^{}]*\}', log.read())
keys = ("input_i", "input_tp", "input_lra", "input_thresh", "target_offset")
stats = None
for block in reversed(blocks):
    try:
        candidate = json.loads(block)
    except ValueError:
        continue
    if all(key in candidate for key in keys):
        stats = candidate
        break
if stats is None:
    sys.exit("Error: ffmpeg did not return loudness measurements.")
values = [float(stats[key]) for key in keys]
if values[1] == -math.inf:
    status = "silent"
elif not all(math.isfinite(value) for value in values):
    status = "unmeasurable"
elif values[0] >= float(sys.argv[2]) - float(sys.argv[3]):
    status = "enough"
else:
    status = "quiet"
print(*values, status)
PY
}

printf 'Measuring: %s\n' "$input"
measure "$input" before
read -r measured_i measured_tp measured_lra measured_thresh offset status \
  <"$work_dir/before.values"
case "$status" in
  silent) printf 'Silent audio; no output created.\n'; exit 0 ;;
  unmeasurable)
    printf 'Audio is too short or too quiet for a reliable measurement; no output created.\n'
    exit 0 ;;
esac
printf 'Before: %s LUFS | true peak %s dBTP\n' "$measured_i" "$measured_tp"
if [[ "$status" == enough ]]; then
  printf 'Already at or above %s LUFS (within %s LU); no output created.\n' \
    "$TARGET_LUFS" "$TOLERANCE_LU"
  exit 0
fi
[[ ! -e "$output" && ! -L "$output" ]] || die "Output already exists: $output"
[[ -d "${output%/*}" ]] || die "Output folder does not exist: ${output%/*}"

printf 'Normalizing toward %s LUFS...\n' "$TARGET_LUFS"
filter="loudnorm=I=$TARGET_LUFS:TP=$TRUE_PEAK_DB:LRA=$LOUDNESS_RANGE"
filter+=":measured_I=$measured_i:measured_TP=$measured_tp:measured_LRA=$measured_lra"
filter+=":measured_thresh=$measured_thresh:offset=$offset:linear=false:print_format=json"
if ! ffmpeg -nostdin -hide_banner -nostats -n -i "$input" \
    -map 0:a:0 -vn -sn -dn -map_metadata -1 -map_chapters -1 \
    -af "$filter" -ar 44100 -ac 2 -c:a libmp3lame -b:a 192k \
    "$work_dir/result.mp3" >/dev/null 2>"$work_dir/encode.log"; then
  cat "$work_dir/encode.log" >&2
  die 'Audio conversion failed.'
fi

# Check the encoded MP3, since encoding can change both loudness and peaks.
measure "$work_dir/result.mp3" after
python3 - "$work_dir/after.values" "$measured_i" "$TRUE_PEAK_DB" <<'PY'
import math
import sys

with open(sys.argv[1]) as readings:
    values = readings.read().split()
loudness, peak = map(float, values[:2])
if not all(math.isfinite(value) for value in (loudness, peak)):
    sys.exit("Error: Output could not be measured; no output saved.")
if peak >= 0:
    sys.exit("Error: Encoded output reaches 0 dBTP; lower TRUE_PEAK_DB and retry. No output saved.")
if loudness <= float(sys.argv[2]):
    sys.exit("Error: Processing did not increase loudness; no output saved.")
print(f"After:  {loudness:.2f} LUFS | true peak {peak:.2f} dBTP")
print(f"Loudness increase: {loudness - float(sys.argv[2]):.2f} LU")
if peak > float(sys.argv[3]) + 0.1:
    print("Note: MP3 encoding raised the true peak slightly above the limiter ceiling.")
PY

# Exclusive creation also protects existing files if another process creates
# the destination after our earlier check. Publish only the verified result.
python3 - "$work_dir/result.mp3" "$output" <<'PY'
import os
import shutil
import sys

created = False
try:
    with open(sys.argv[1], "rb") as source, open(sys.argv[2], "xb") as destination:
        created = True
        shutil.copyfileobj(source, destination)
except OSError as error:
    if created:
        os.unlink(sys.argv[2])
    sys.exit(f"Error: Could not save output: {error}")
PY
printf 'Saved: %s\n' "$output"
